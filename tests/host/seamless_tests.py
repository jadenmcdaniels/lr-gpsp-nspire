#!/usr/bin/env python3
"""S9: the scenario matrix for the seamless USB port (S1 observer, S3
arbiter, S4 bus power cycle, S7 toasts), on the simulated calculator
(build/gpsp-host, host_usbpad.c's simulators; the calculator rows are two
gpsp-host processes on a socket, pad_tests.py's swap pattern).

    seamless_tests.py [NAME...]    all tests, or the ones named

The matrix: {empty, pad, calc, drive} x {at start, during play} x
{unplug, replug, swap}: 24 cells, each its own gpsp-host run (a calculator
cell is two runs, once with the calculator under test as player 1 and once
as player 2, both scripted alike). "At start" is a device that is on the
bus from the first moment and that the OS never offers to the driver (the
driver registered after it: usbshim.h:16-18; script word `pad preplug`),
"during play" one plugged in while the game runs (offered at once). Each cell
asserts what the plan's S9 lists: the state the arbiter ends in, which stage
resolved it (offered by the OS / bus power cycle N / ...), the toasts (S7),
keys released on unplug, the link ended on unplug, no replug hint, and the OS
sleep counters (S8) where the port ends EMPTY. A cell reads:

    empty: nothing on the port stays silent however the cable and adapter
           are moved: unplug = nothing to take out, replug = a blip shorter
           than a look, swap = the adapter end and the device end in turn
    pad:   unplug = the pad and its adapter go (a second variant: the pad
           only, the adapter staying in the calculator), replug = out and in
           again, swap = pad -> flash drive
    calc:  unplug = the cable comes out, replug = out and in (both
           calculators pull and put it back, in step: `pad sync`), swap =
           calculator -> pad
    drive: unplug / replug as the pad, swap = drive -> pad
and the swaps the other way round (pad -> calc, drive -> calc) as extra cells.

Cells that S5 (resident driver) or S6 (own host stack) would resolve assert
what the code does today: those stages do not exist, stage 3 (direct read)
passes with nothing wired, and the last-resort toast shows once. They are
RESIDENT_TODAY and DIRECT_TODAY (cell_resident_today, cell_direct_today);
flip the expectations there when S5/S6 land.

Runs are in build/host_tests/sm_*."""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H  # noqa: E402
import pad_tests as P  # noqa: E402

START = P.START_GAME           # 2.25 s: the game runs
KIND = {"pad": "xinput", "drive": "drive"}
CONNECTED, GONE = "Controller connected (Xbox 360)", "Controller disconnected"
# R11w part 2 (2026-11-16): the last resort's words plainer; the arbiter no
# longer says to swap the cable (it came with controllers: the reference
# report); SWAP_CABLE stays as a text that must never show
LAST_RESORT, STARTING = "Unplug the USB device and plug it in again", "Starting the USB device..."
SWAP_CABLE = "Swap the cable: adapter to the other calculator"
# DETECT: the come-up's message for a device at the adapter's end that nobody
# claims for 1 s (nspire_link.c BRING_HOST_QUIET_MS): a pad or drive the OS
# never offered, until the bus power cycle brings it or the stages give up.
COMEUP = "USB device in: connecting..."
# PAD3 R1: the Controller page's toast followed the arbiter's stage 4, again
# 20 s after the first. R11w part 2: one message that asks something per
# plug: after the last resort, no page toast (where the arbiter said nothing,
# the page's toast is that one message).
PAGE = P.H_TOAST

STRAY = (LAST_RESORT, STARTING, SWAP_CABLE, "Plug the controller in again", "Other calculator found: start gpSP there",
         "The USB port has a controller in it", "isn't linking")
DUMP = bool(os.environ.get("SEAMLESS_DUMP"))


# ---------------------------------------------------------------- reading a run

class View:
    """What one gpsp-host run says: the toasts, the arbiter's holders and
    stages, the pad driver's events, the cycles."""

    def __init__(self, run):
        self.run = run
        self.lines = P.link_log(run)
        self.osd = P.osd(run)
        self.texts = [t for _, t in self.osd]
        g = lambda rx: [m.group(1) for l in self.lines for m in [re.search(rx, l)] if m]
        # the holders from pad.txt (nspire_arbiter.c's host-only note), for the same reason as the cycles below:
        # with the driver's offer lines (match-fix) the link log's 3 KB tail no longer reaches the first holders
        self.holders = [m.group(1) for _, t in P.pad_log(run) for m in [re.match(r"arbiter: port owner (\w+) \(was", t)] if m]
        self.firm = [o for o in self.holders if o != "WAIT"]   # the holders a state of its own (WAIT passes by in a look)
        self.ports = g(r"seamless: port (EMPTY|HOST_END|DEVICE_END|UNKNOWN) \(bits")
        self.stages = [int(x) for x in g(r"seamless: stage (\d)")]
        # the cycles that ran (r >= 0) from pad.txt (the simulator's own "reenum cycle N"): the link log
        # keeps only its last 3 KB before a new link (nspire_link.c LOG_KEPT_TAIL), a cycle at start can be older
        self.cycles = len([1 for _, t in P.pad_log(run) if re.match(r"reenum cycle \d", t)])
        self.refused = len(g(r"(seamless: cycle \d refused)"))
        self.attach = len(g(r"(pad: event: attach)"))
        self.detach = len(g(r"(pad: event: detach)"))
        self.stats = P.stat_lines(run)

    def n(self, text):
        return self.texts.count(text)

    def stray(self):
        return [t for t in self.texts if any(s in t for s in STRAY)]

    def holder(self):
        return self.holders[-1] if self.holders else "NONE"

    def port(self):
        return self.ports[-1] if self.ports else "EMPTY"

    def sleeps_last(self):
        """OS sleeps between the last two 'pad stat' lines."""
        return self.stats[-1]["sleeps"] - self.stats[-2]["sleeps"] if len(self.stats) >= 2 else None

    def time_of(self, prefix):
        return [t for t, text in P.pad_log(self.run) if text.startswith(prefix)]

    def dump(self, name):
        if DUMP:
            print("  %s: holders %s stages %s cycles %d refused %d attach %d detach %d port %s sleeps_last %s\n    toasts %s" % (
                name, self.holders, self.stages, self.cycles, self.refused, self.attach, self.detach, self.port(),
                self.sleeps_last(), self.osd))


def hold_peek(name):
    """Script lines: Pad A held for a moment, the game's buttons read."""
    return "hold Pad_A\nwait 0.3\npeek %s 4000130:2\n" % name


def keys(run, name):
    return P.p1(run, name)


STAT = "pad stat\n"
QUIET = STAT + "wait 5\n" + STAT   # the last 5 s' counters (the port is settled)


# ---------------------------------------------------------------- single device

def plug(dev, when):
    """The script that puts dev on the port and waits for it to settle: at
    start (on the bus, never offered, the bus power cycle brings it), or
    during play (offered at once)."""
    if when == "start":
        return "pad preplug %s\npad cycle ok\nwait 8\n" % KIND[dev]
    return "wait 3\npad in %s\nwait 4\n" % KIND[dev]


def unplug_script(dev, adapter_stays=False):
    """The pad (and its adapter) comes out; keys held at that moment must be
    released. The port is read for 5 s after the 14 s the stages would take."""
    s = ""
    if dev == "pad":
        s += hold_peek("k1")
    s += "pad out%s\n" % (" drive" if dev == "drive" else "")
    if adapter_stays:
        s += "pad port 1\n"
    s += "wait 0.3\n"
    if dev == "pad":
        s += "peek k2 4000130:2\nrelease Pad_A\n"
    return s + "wait 14\n" + QUIET


def check_unplug(name, v, run, dev, adapter_stays=False):
    """After the unplug: keys released, the toasts, the port and its holder,
    and (the port EMPTY) no OS time. With the adapter staying in, nobody owns
    the port and no stage toast says the cable is wrong (S9 fix: the arbiter's
    'claimed' flag)."""
    if dev == "pad":
        H.check(keys(run, "k1") == "A" and keys(run, "k2") == "none",
                "%s: the pad's key reaches the game, and is released when the pad goes: %s then %s" % (
                    name, keys(run, "k1"), keys(run, "k2")))
        H.check(v.n(GONE) == 1 and v.detach == 1, "%s: one 'disconnected' toast: %s" % (name, v.texts))
    if adapter_stays:
        H.check(v.port() == "HOST_END", "%s: the adapter stays: port %s" % (name, v.port()))
        H.check(v.holder() == "WAIT", "%s: nobody owns the port: %s" % (name, v.holders))
    else:
        H.check(v.port() == "EMPTY" and v.holder() == "NONE", "%s: the port ends EMPTY, holder NONE: %s %s" % (
            name, v.port(), v.holders))
        H.check(v.sleeps_last() == 0, "%s: no OS sleep in the last 5 s: %s" % (name, v.sleeps_last()))
    H.check(not v.stray(), "%s: no stage toast after the unplug: %s" % (name, v.texts))


def cell_single(dev, when, action):
    """A pad or a drive: plugged in at start or during play, then taken out,
    taken out and put back, or swapped for the other kind of device."""
    name = "sm_%s_%s_%s" % (dev, when, action)
    mine = "PAD" if dev == "pad" else "OTHER"
    out = "pad out%s\n" % (" drive" if dev == "drive" else "")
    first = plug(dev, when) + STAT
    if action == "unplug":
        script = first + unplug_script(dev)
    elif action == "replug":
        script = (first + unplug_script(dev).replace("wait 14\n" + QUIET, "wait 2\n") + "pad in %s\n" % KIND[dev] +
                  (hold_peek("k3") + "release Pad_A\n" if dev == "pad" else "") + "wait 14\n" + QUIET)
    else:   # the other kind of device: out, a second, the other in
        then = "drive" if dev == "pad" else "pad"
        script = first + out + "wait 1\npad in %s\nwait 14\n" % KIND[then] + QUIET
    r = H.Run(name).run(START + script + "quit\n")
    v = View(r)
    v.dump(name)
    # how the first plug was resolved: preplugged = the bus power cycle 1 (stages 1, 2, and 3 and 4 never
    # run), plugged in during play = offered by the OS at once, no stage at all
    if when == "start":
        H.check(v.cycles == 1 and v.stages == [1, 2], "%s: resolved by bus power cycle 1 (stages %s, cycles %d)" % (name, v.stages, v.cycles))
    else:
        H.check(v.cycles == 0 and not v.stages, "%s: offered by the OS, no cycle, no stage (stages %s, cycles %d)" % (
            name, v.stages, v.cycles))
    H.check(not v.stray() and not any("Plug the controller" in t for t in v.texts), "%s: no stage or replug toast: %s" % (name, v.texts))
    # DETECT: at start the device waits ~3 s for the cycle: the come-up's message once, first; during play none
    H.check(v.n(COMEUP) == (1 if when == "start" else 0) and (when != "start" or v.texts[:1] == [COMEUP]),
            "%s: the come-up's message once at start, none during play: %s" % (name, v.texts))
    if action == "unplug":
        H.check(v.firm == [mine, "NONE"], "%s: holders %s then NONE: %s" % (name, mine, v.holders))
        check_unplug(name, v, r, dev)
    elif action == "replug":
        H.check(v.firm == [mine, "NONE", mine], "%s: holders %s, NONE, %s again: %s" % (name, mine, mine, v.holders))
        if dev == "pad":
            H.check(v.n(CONNECTED) == 2 and v.n(GONE) == 1 and v.attach == 2,
                    "%s: connected twice, disconnected once: %s" % (name, v.texts))
            H.check(keys(r, "k1") == "A" and keys(r, "k2") == "none" and keys(r, "k3") == "A",
                    "%s: keys: held, released on unplug, back after the replug: %s %s %s" % (
                        name, keys(r, "k1"), keys(r, "k2"), keys(r, "k3")))
        else:
            H.check([t for t in v.texts if t != COMEUP] == [], "%s: a drive makes no toast (but the come-up's): %s" % (name, v.texts))
        H.check(v.port() == "HOST_END" and v.holder() == mine, "%s: the port is the device's again: %s %s" % (name, v.port(), v.holder()))
    else:
        then_owner = "OTHER" if dev == "pad" else "PAD"
        H.check(v.firm in ([mine, "NONE", then_owner], [mine, then_owner]), "%s: holders %s then %s: %s" % (name, mine, then_owner, v.holders))
        if dev == "pad":
            H.check(v.n(GONE) == 1 and v.n(CONNECTED) == 1, "%s: the pad's goodbye, no new pad: %s" % (name, v.texts))
        else:
            H.check(v.n(CONNECTED) == 1 and v.attach == 1 and v.n(GONE) == 0, "%s: then the pad: %s" % (name, v.texts))
        H.check(v.holder() == then_owner and v.port() == "HOST_END", "%s: ends with the %s: %s" % (name, then_owner, v.holders))


# ---------------------------------------------------------------- empty port

def cell_empty(when, action):
    name = "sm_empty_%s_%s" % (when, action)
    lead = "wait 10\n" if when == "play" else ""
    blip = lambda port: "pad port %d\nwait 0.05\npad port 0\nwait 9\n" % port
    act = {"unplug": "pad out\npad out drive\nwait 1\n", "replug": blip(1) + blip(1),
           "swap": blip(1) + blip(2) + blip(1)}[action]
    r = H.Run(name).run(START + lead + STAT + act + "wait 15\n" + QUIET + "quit\n")
    v = View(r)
    v.dump(name)
    H.check(v.texts == [], "%s: no toast for an empty port: %s" % (name, v.texts))
    H.check(v.port() == "EMPTY" and v.holder() == "NONE", "%s: EMPTY, nobody owns it: %s %s" % (name, v.port(), v.holders))
    H.check(not v.stages and v.cycles == 0 and v.refused == 0, "%s: no stage, no cycle: %s" % (name, v.stages))
    H.check(v.sleeps_last() == 0, "%s: no OS sleep in the last 5 s: %s" % (name, v.sleeps_last()))
    H.check(v.attach == 0 and v.detach == 0, "%s: no pad events" % name)


# ---------------------------------------------------------------- calculators

class Duo:
    """Two gpsp-host processes on a socket, scripted in step: both(line) goes
    to both, sub(line) to the one under test, pre(line) to it before the game
    starts, sync() makes them meet (nothing linked, a link waits on the other's
    frames) before the next line. The cable is one: the test's `pad calc
    out|in` is seen by the other calculator too (host_link.c CTRL_UNPLUG)."""

    def __init__(self, name, who):
        self.name, self.who = name, who
        self.s = {1: [], 2: []}
        self.first = {1: "", 2: ""}
        self.syncs = 0
        self.start = START

    def pre(self, text, both=False):
        for p in ((1, 2) if both else (self.who,)):
            self.first[p] += text

    def both(self, text):
        for p in (1, 2):
            self.s[p].append(text)

    def sub(self, text):
        self.s[self.who].append(text)

    def sub_t(self, text):
        """The test's lines, and the same time passing on the other side."""
        self.sub(text)
        self.s[3 - self.who].append("wait %g\n" % sum(float(x) for x in re.findall(r"^wait ([\d.]+)$", text, re.M)))

    def sync(self):
        self.syncs += 1
        prefix = os.path.join(H.WORK, "%s.sy%d" % (self.name, self.syncs))
        for p in (1, 2):
            for q in (1, 2):
                try:
                    os.remove("%s.%d" % (prefix, q))
                except OSError:
                    pass
            self.s[p].append("pad sync %s %d\n" % (prefix, p))

    def run(self):
        runs = {p: H.Run("%s_p%d" % (self.name, p)) for p in (1, 2)}
        sock = os.path.join(H.WORK, self.name + ".sock")
        if os.path.exists(sock):
            os.remove(sock)
        procs = {p: runs[p].start(self.first[p] + self.start + "".join(self.s[p]) + "quit\n", "Pattern.gba.tns",
                                  ["--link", sock, "--player", str(p), "--link-auto"]) for p in (1, 2)}
        for p in (1, 2):
            runs[p].finish(procs[p])
        return View(runs[self.who]), View(runs[3 - self.who])


def linked(v):
    return len([t for t in v.texts if t.startswith("Linked:")])


def calc_plan(d, when):
    """The cable in from t0 (at start) or out at first and in after 3 s
    (during play); both sides, in step."""
    if when == "play":
        d.pre("pad calc out\n", both=True)   # no cable at the start, on either side
        d.both("wait 3\n")
        d.sync()
        d.sub("pad calc in\n")
    d.both("wait 9\n")


def check_cable_out(name, vs):
    for v, side in zip(vs, ("test", "other")):
        H.check(any("The link cable came out" in l for l in v.lines), "%s (%s): the link ended with the cable" % (name, side))
        H.check(v.port() == "EMPTY" and v.holder() == "NONE", "%s (%s): the port ends EMPTY: %s %s" % (name, side, v.port(), v.holders))
        H.check(v.sleeps_last() == 0, "%s (%s): no OS sleep in the last 5 s: %s" % (name, side, v.sleeps_last()))
        H.check(not v.stray(), "%s (%s): no stray toast: %s" % (name, side, v.texts))


def cell_calc(when, action, who):
    name = "sm_calc_%s_%s_p%d" % (when, action, who)
    d = Duo(name, who)
    calc_plan(d, when)
    if action == "unplug":
        d.sub("pad calc out\n")
        d.both("wait 1\n" + QUIET)
    elif action == "replug":
        d.sub("pad calc out\n")
        d.both("wait 2\n")
        d.sync()
        d.sub("pad calc in\n")
        d.both("wait 9\n")
        # DETECT: the link now comes 5 s after the plug (the OS's node, host_link.c CALC_NODE_PLUG_S): the
        # cable pulled at the end, so that neither side's quit ends the other's link first (a race in wall time)
        d.sub("pad calc out\n")
        d.both("wait 1\n")
    else:
        d.sub("pad calc out\n")
        d.both("wait 1\n")
        d.sub_t("pad in xinput\n" + hold_peek("k1") + "wait 12\n" + STAT + "wait 5\n" + STAT)
    t, o = d.run()
    t.dump(name + " test")
    o.dump(name + " other")
    mine = "LINK"
    for v in (t, o):   # the node was there and the link came by itself: no stage, no power cycle
        H.check(v.cycles == 0 and not v.stages and not v.stray(), "%s: no stage, no power cycle, no stray toast: %s %d %s" % (
            name, v.stages, v.cycles, v.stray()))
    if action == "unplug":
        for v in (t, o):
            H.check(linked(v) == 1, "%s: Linked once: %s" % (name, v.texts))
        check_cable_out(name, (t, o))
        H.check(t.firm == [mine, "NONE"], "%s: holders LINK, NONE: %s" % (name, t.holders))
    elif action == "replug":
        for v in (t, o):
            H.check(linked(v) == 2 and v.n("The other calculator isn't linking.") == 0,
                    "%s: Linked twice, each time at once: %s" % (name, v.texts))
            H.check(v.firm == [mine, "NONE", mine, "NONE"], "%s: holders %s" % (name, v.holders))
            H.check(any("The link cable came out" in l for l in v.lines), "%s: the link ended on the unplug" % name)
            H.check(not v.stray(), "%s: no stray toast: %s" % (name, v.texts))
    else:
        H.check(linked(t) == 1 and t.n(CONNECTED) == 1, "%s: Linked once, then the pad: %s" % (name, t.texts))
        H.check(t.firm == ["LINK", "NONE", "PAD"], "%s: holders LINK, NONE, PAD: %s" % (name, t.holders))
        H.check(any("The link cable came out" in l for l in t.lines), "%s: the link ended with the cable" % name)
        H.check(keys(t.run, "k1") == "A", "%s: the pad's key reaches the game: %s" % (name, keys(t.run, "k1")))
        H.check(t.cycles == 0 and not t.stages and not t.stray(), "%s: no stage, no stray toast: %s %s" % (name, t.stages, t.texts))
        H.check(t.port() == "HOST_END", "%s: the pad's adapter: %s" % (name, t.port()))


def cell_from_pad_or_drive_to_calc(dev, when, who):
    """Extra cells: the device is taken out and the calculator's cable put
    in, the cable end where the adapter was."""
    name = "sm_%s_%s_to_calc_p%d" % (dev, when, who)
    d = Duo(name, who)
    d.pre("pad calc out\n", both=True)
    d.sub_t(plug(dev, when) + (hold_peek("k1") + "release Pad_A\n" if dev == "pad" else ""))
    d.both("wait 1\n")
    d.sub("pad out%s\n" % (" drive" if dev == "drive" else ""))
    d.both("wait 1\n")
    d.sync()
    d.sub("pad calc in\n")
    d.both("wait 8\n")
    d.sub("pad calc out\n")   # DETECT: as in cell_calc's replug: the end of the link not left to the quits' race
    d.both("wait 1\n")
    t, o = d.run()
    t.dump(name)
    mine = "PAD" if dev == "pad" else "OTHER"
    H.check(t.firm == [mine, "NONE", "LINK", "NONE"], "%s: holders %s, NONE, LINK (NONE: pulled at the end): %s" % (name, mine, t.holders))
    H.check(linked(t) == 1 and t.n("The USB port has a controller in it") == 0, "%s: linked, no refusal: %s" % (name, t.texts))
    H.check(not t.stray(), "%s: no stray toast: %s" % (name, t.texts))
    H.check(t.n(CONNECTED) == (1 if dev == "pad" else 0) and t.n(GONE) == (1 if dev == "pad" else 0), "%s: the pad's toasts: %s" % (name, t.texts))
    H.check(linked(o) == 1 and o.firm[-2:] == ["LINK", "NONE"], "%s: the other calculator linked too: %s %s" % (name, o.texts, o.holders))
    if when == "start":
        H.check(t.cycles == 1, "%s: the preplugged device was brought by cycle 1: %d" % (name, t.cycles))
    else:
        H.check(t.cycles == 0, "%s: no cycle: %d" % (name, t.cycles))


# ---------------------------------------------------------------- stages that don't exist yet

def last_resort_round(name, extra=""):
    """A preplugged pad that no stage brings in: what S5/S6 would resolve."""
    r = H.Run(name).run(START + "pad preplug xinput\npad cycle ignored\n" + extra + "wait 20\n" + STAT + "wait 5\n" + STAT + "quit\n")
    return View(r)


def cell_resident_today():
    """RESIDENT_TODAY: a pad that was on the bus before gpSP started, the OS
    never offers it, the bus power cycle does nothing, and no resident driver
    (S5) holds it: the stages run out (1 power cycle, 2 wait, 3 direct, 4 gave up), the
    'Starting' toast and the last resort show once. With S5 the pad connects
    at the first poll with no toast: then expect CONNECTED once, no stage."""
    name = "sm_resident_today"
    v = last_resort_round(name)
    v.dump(name)
    H.check(v.stages == [1, 2, 3, 4] and v.cycles == 2, "%s: the stages 1 2 3 4 and two cycles: %s %d" % (name, v.stages, v.cycles))
    # (R11w part 2: was [..., LAST_RESORT, PAGE, PAGE]: three messages that ask
    # something for one stuck device; once per plug now)
    H.check(v.texts == [COMEUP, STARTING, LAST_RESORT],
            "%s: the come-up's, 'Starting', the last resort once, nothing more this plug: %s" % (name, v.texts))
    H.check(v.attach == 0 and v.holder() == "WAIT", "%s: no pad: %s %s" % (name, v.attach, v.holders))
    H.check(any("seamless: stage 4 (gave up)" in l for l in v.lines), "%s: 'gave up' logged" % name)
    H.check(not any("Plug the controller" in t for t in v.texts), "%s: no replug hint" % name)


def cell_direct_today():
    """DIRECT_TODAY: as above with a device whose descriptor could be read
    ('pad direct'): stage 3 is an empty hook until S6, so nothing is read and
    nothing classifies the device. With S6 expect the pad (class 3 / xinput)
    kept, or a TI device handed back to the OS."""
    name = "sm_direct_today"
    r = H.Run(name).run(START + "pad preplug xinput\npad cycle ignored\npad direct\nwait 20\npad readdesc\nquit\n")
    v = View(r)
    v.dump(name)
    H.check(v.stages == [1, 2, 3, 4] and v.attach == 0, "%s: stage 3 passes with nothing wired, no pad: %s %d" % (name, v.stages, v.attach))
    H.check(v.texts == [COMEUP, STARTING, LAST_RESORT], "%s: the last resort once, nothing more this plug (R11w part 2): %s" %
            (name, v.texts))
    H.check("readdesc: ok 045e:028e" in [t for _, t in P.pad_log(r)], "%s: the device is readable, nobody read it before the test did" % name)
    d = H.Run(name + "_drive").run(START + "pad preplug drive\npad cycle ignored\nwait 20\nquit\n")
    vd = View(d)
    H.check(vd.texts == [COMEUP, STARTING, LAST_RESORT] and vd.holder() == "WAIT", "%s: a drive nobody claims: the same: %s %s" % (name, vd.texts, vd.holders))


def test_failure_rows():
    """Cycle ignored and nothing else brings the device: 'gave up', the last
    resort once; after the user's unplug and replug, again once, and the pad
    connects; a second cycle that works; a cycle the controller refuses."""
    cell_resident_today()
    cell_direct_today()
    # the user does what the toast says: unplug, then plug in again (hot)
    r = H.Run("sm_fail_user").run(START + "pad preplug xinput\npad cycle ignored\nwait 16\npad out\nwait 2\npad in xinput\nwait 5\n"
                                  "pad preplug xinput\npad cycle ignored\npad out\nwait 1\npad preplug xinput\nwait 16\nquit\n")
    v = View(r)
    v.dump("sm_fail_user")
    H.check(v.texts.count(LAST_RESORT) == 2 and v.texts.count(STARTING) == 2 and v.n(CONNECTED) == 1 and v.n(COMEUP) == 2,
            "fail user: each toast once per port change (2 changes), the replugged pad connects: %s" % v.texts)
    # a bus power cycle that works only the second time
    r = H.Run("sm_fail_late").run(START + "pad preplug xinput\npad cycle late 2\nwait 16\nquit\n")
    v = View(r)
    v.dump("sm_fail_late")
    H.check(v.cycles == 2 and v.texts == [COMEUP, STARTING, CONNECTED] and not any(i == 4 for i in v.stages),
            "fail late: cycle 2 resolved it, 'Starting' then the pad, no last resort: %s %d %s" % (v.stages, v.cycles, v.texts))
    # nothing to cycle (the adapter alone). R11w part 2: this checked the cable
    # hint then the page's toast; with nothing on the wires the arbiter says
    # nothing now (the swap came with controllers: the reference report), and
    # the page's toast for an adapter that never had a device is the one
    # message (at 8 s)
    r = H.Run("sm_fail_alone").run(START + "pad port 1\nwait 16\nquit\n")
    v = View(r)
    H.check(v.texts == [PAGE] and v.cycles == 0 and v.refused == 1,
            "fail alone: nothing connected, nothing to cycle: only the page's toast: %s %d" % (v.texts, v.cycles))


def pad_lines(run, prefix):
    return [t for _, t in P.pad_log(run) if t.startswith(prefix)]


def test_loops():
    """20 replug loops stay stable: each loop resolves the same way, one toast
    pair each, no growing state (the cycle count restarts per port change), the
    port ends EMPTY and quiet. The loops are 10+ s apart so that the flap
    detector's 8 s gap (nspire_link.c FLAP_GAP_MS: 'USB port keeps resetting')
    never sees them as one. Counted from pad.txt: 20 loops overrun the 12 KB
    link log."""
    wait = "wait 9\n"
    # hot plug, the whole thing out each time
    r = H.Run("sm_loop_hot").run(START + ("pad in xinput\nwait 4\npad out\n" + wait) * 20 + QUIET + "quit\n")
    v = View(r)
    H.check(v.texts == [CONNECTED, GONE] * 20, "loop hot: 20 connects and 20 disconnects, nothing else: %d toasts %s" % (len(v.texts), v.stray()))
    H.check(not pad_lines(r, "reenum"), "loop hot: no bus power cycle")
    H.check(v.sleeps_last() == 0, "loop hot: ends EMPTY and quiet: %s" % v.sleeps_last())
    H.check(r.exit == 0, "loop hot: exit %d" % r.exit)
    # the pad pulled out of an adapter that stays in, 20 times
    r = H.Run("sm_loop_adapter").run(START + "pad port 1\n" + ("pad in xinput\nwait 4\npad out\n" + wait) * 20 + "quit\n")
    v = View(r)
    H.check(v.texts == [CONNECTED, GONE] * 20, "loop adapter stays: 20 connects, 20 disconnects and no stage toast: %d toasts %s" % (
        len(v.texts), v.stray()[:2]))
    # a pad the OS offers only after the bus power cycle, 20 times
    r = H.Run("sm_loop_cycle").run(START + ("pad preplug xinput\npad cycle ok\nwait 5\npad out\n" + wait) * 20 + QUIET + "quit\n")
    v = View(r)
    cyc = pad_lines(r, "reenum")
    H.check(v.texts == [COMEUP, CONNECTED, GONE] * 20 and cyc == ["reenum cycle 1"] * 20,
            "loop cycle: 20 pads, cycle 1 each time (the count restarts per port change): %d toasts, cycles %s" % (len(v.texts), sorted(set(cyc))))
    H.check(v.sleeps_last() == 0, "loop cycle: ends EMPTY and quiet: %s" % v.sleeps_last())
    # a drive, 20 times
    r = H.Run("sm_loop_drive").run(START + ("pad in drive\nwait 4\npad out drive\n" + wait) * 20 + QUIET + "quit\n")
    v = View(r)
    H.check(v.texts == [] and v.sleeps_last() == 0 and not pad_lines(r, "reenum"),
            "loop drive: no toast, no cycle, ends EMPTY and quiet: %s %s" % (v.texts[:2], v.sleeps_last()))
    # the calculator's cable, 20 times, both sides in step
    d = Duo("sm_loop_calc", 1)
    d.both("wait 3\n")
    for _ in range(20):
        d.sub("pad calc out\n")
        d.both("wait 1\n")
        d.sync()
        d.sub("pad calc in\n")
        d.both("wait 9\n")
    t, o = d.run()
    for v, side in ((t, "test"), (o, "other")):
        H.check(linked(v) == 21 and v.n("The other calculator isn't linking.") == 0 and v.n("The link cable came out.") >= 20,
                "loop calc (%s): linked 21 times (start + 20 replugs), the cable came out 20 times or more (the exit of the other): %d %d %s" % (
                    side, linked(v), v.n("The link cable came out."), v.texts[-3:]))
        H.check(not v.stray(), "loop calc (%s): no stray toast: %s" % (side, v.stray()))


# ---------------------------------------------------------------- the matrix

def test_matrix_empty():
    for when in ("start", "play"):
        for action in ("unplug", "replug", "swap"):
            cell_empty(when, action)


def test_matrix_pad():
    for when in ("start", "play"):
        for action in ("unplug", "replug", "swap"):
            cell_single("pad", when, action)


def test_matrix_drive():
    for when in ("start", "play"):
        for action in ("unplug", "replug", "swap"):
            cell_single("drive", when, action)


def test_matrix_calc():
    for who in (1, 2):
        for when in ("start", "play"):
            for action in ("unplug", "replug", "swap"):
                cell_calc(when, action, who)


def test_swaps_extra():
    """The swaps from the pad and the drive to the calculator, both ways
    round, at start and during play; and the pad's unplug with its adapter
    staying (what a user does)."""
    for who in (1, 2):
        for dev in ("pad", "drive"):
            for when in ("start", "play"):
                cell_from_pad_or_drive_to_calc(dev, when, who)
    for when in ("start", "play"):
        name = "sm_pad_%s_unplug_adapter" % when
        r = H.Run(name).run(START + plug("pad", when) + STAT + unplug_script("pad", adapter_stays=True) + "quit\n")
        v = View(r)
        v.dump(name)
        check_unplug(name, v, r, "pad", adapter_stays=True)
        H.check(v.cycles == (1 if when == "start" else 0), "%s: cycles %d" % (name, v.cycles))


def test_docs():
    """S11/S10: the docs match the code. The constants in nspire_arbiter.h
    and nspire_usbport.c appear in docs/seamless-usb.txt, the toast texts
    there exist in the sources, the hardware test sheet (when given) names
    the limits, no user doc says 'Plug it in again', and
    tools/seamless_log_check.py runs on a host log, a log without seamless
    lines, and the calculator logs (when given)."""
    import subprocess
    root = os.path.normpath(os.path.join(HERE, "..", ".."))
    rd = lambda *p: open(os.path.join(root, *p), encoding="utf-8", errors="replace").read()
    sec = rd("docs", "seamless-usb.txt")
    hdr = rd("nspire_arbiter.h")
    for name in ("ARB_STAGE0_MS", "ARB_STAGE2_MS", "ARB_CYCLE2_AT_MS", "ARB_CYCLES_MAX"):
        val = re.search(r"#define %s\s+(\d+)" % name, hdr).group(1)
        H.check(any(name in l and val in l for l in sec.split("\n")), "docs: %s = %s in the notes" % (name, val))
    H.check(re.search(r"SAMPLE_FRAMES (\d+)", sec).group(1) in rd("nspire_usbport.c"), "docs: SAMPLE_FRAMES")
    H.check("NODE_HINT_MS" in sec and "#define NODE_HINT_MS 10000" in rd("nspire_link.c"), "docs: NODE_HINT_MS 10 s")
    src = "".join(rd(f) for f in os.listdir(root) if (f.endswith(".c") or f.endswith(".h")) and f.startswith("nspire_"))
    # R11w part 2 (2026-11-16): the last resort's words plainer ("Unplug and
    # replug the USB device" before, now nspire_arbiter.h's ARB_REPLUG_TEXT),
    # and the arbiter's swap hint gone (it came with controllers)
    for toast in ("Starting the USB device...", LAST_RESORT, "Controller disconnected",
                  "Other calculator found: start gpSP there"):
        H.check(toast in sec and toast in src, "docs: toast %r in the notes and in the sources" % toast)
    test_sheet = os.environ.get("GPSP_SEAMLESS_TEST_SHEET", "")
    test_txt = open(test_sheet).read() if test_sheet and os.path.exists(test_sheet) else None
    if test_txt is not None:
        H.check("within 3 seconds" in test_txt and "within 5 seconds" in test_txt and "Linked: player" in test_txt, "docs: the hardware test sheet's limits")
    for f in ("README.md", "README.txt"):
        H.check("plug it in again" not in rd(f).lower(), "docs: no 'Plug it in again' in %s" % f)
    tool = os.path.join(root, "tools", "seamless_log_check.py")

    def run(path):
        return subprocess.run([sys.executable, "-I", tool, path], capture_output=True, text=True)
    r = H.Run("s10_tool").run(START + "pad in xinput\nwait 0.5\npress Pad_A\nwait 0.3\npad out\nwait 0.5\nquit\n")
    o = run(r.save("gpsp_link.txt.tns"))
    H.check(o.returncode == 0 and "device offered: 045e:028e, class Xbox 360" in o.stdout and "first report: slot 0" in o.stdout
            and "buttons seen: east" in o.stdout and "detach: slot 0" in o.stdout, "tool: the pad's story: %s" % o.stdout[-700:])
    quiet = os.path.join(H.WORK, "s10_quiet_log.txt")
    open(quiet, "w").write("     0 gpSP build x\n   120 OS sleep: PC test build, 0 sleeps\ngarbage line\n")
    o = run(quiet)
    H.check(o.returncode == 0 and "Traceback" not in o.stderr and "no 'seamless:' lines" in o.stdout, "tool: a log with no seamless lines: rc %d" % o.returncode)
    r4 = os.environ.get("GPSP_SEAMLESS_LOGS", "")
    if r4 and os.path.isdir(r4):
        bad = [f for f in os.listdir(r4) if f.endswith(".tns") and (run(os.path.join(r4, f)).returncode != 0)]
        H.check(not bad, "tool: the r4 calculator logs (no seamless lines) give no crash: %s" % bad)
    r = H.Run("s10_tool_slow").run(START + "pad port 1\nwait 20\nquit\n")
    o = run(r.save("gpsp_link.txt.tns"))
    H.check(o.returncode == 1 and "GAVE UP" in o.stdout, "tool: gave up exits 1: rc %d" % o.returncode)


def test_seam_cells():
    """The cells SEAM-1..13 that the simulator can show:
    the power cycle runs nspire_usbcycle.c on host_usbpad.c's registers, a
    calculator on the cable at this (the host's) end is a connected device
    there, and a cycle that drops its bus logs 'calc: bus power dropped'."""
    dropped = lambda run: [t for t in pad_lines(run, "calc: bus power dropped")]
    # SEAM-1: a CX whose timers don't run (--timer-dead 1): no cycle (its
    # wait would never end with PP 0), the run goes on, the replug toast
    r = H.Run("sm_rv_timer_dead").run(START + "pad port 3\npad preplug xinput\npad cycle ok\nwait 14\nquit\n",
                                      options=["--model", "cx", "--timer-dead", "1"])
    v = View(r)
    v.dump("sm_rv_timer_dead")
    H.check(any("refused: the timer doesn't run" in l for l in v.lines) and not pad_lines(r, "reenum cycle"),
            "SEAM-1: dead timer: the cycle refused, nothing written: %s" % [l for l in v.lines if "reenum" in l])
    H.check(LAST_RESORT in v.texts and SWAP_CABLE not in v.texts, "SEAM-1: the replug toast, not the cable's: %s" % v.texts)
    H.check("the OS ran alone" not in open(r.save("gpsp_pad.txt.tns")).read(),
            "SEAM-1: no blocking discovery burst without a running timer")
    # SEAM-2: the cable put in during play, the other calculator's node
    # listed only 2 s later: no cycle drops its bus, the link comes
    d = Duo("sm_rv_node_late", 1)
    d.pre("pad calc out\n", both=True)
    d.pre("pad calc delay 2\n")
    d.both("wait 3\n")
    d.sync()
    d.sub("pad calc in\n")
    d.both("wait 10\n")
    t, o = d.run()
    t.dump("sm_rv_node_late")
    H.check(not dropped(t.run) and t.cycles == 0, "SEAM-2: a node 2 s late: no power cycle under it: %s %d" % (dropped(t.run), t.cycles))
    H.check(linked(t) == 1 and linked(o) == 1 and not t.stray(), "SEAM-2: linked on both, no stray toast: %s / %s" % (t.texts, o.texts))
    # SEAM-3: the cable in before start, 4 s in the file list: no cycle there
    # (a regression check only: gpsp-host's link is up from its start, so
    # the bug shows in frontend_tests' arb_keys, not here)
    d = Duo("sm_rv_filelist", 1)
    d.start = "wait 4\npress enter\nwait 1\n"
    d.both("wait 8\n")
    t, o = d.run()
    for v, side in ((t, "test"), (o, "other")):
        H.check(not dropped(v.run) and v.cycles == 0 and not v.stray() and linked(v) == 1,
                "SEAM-3 (%s): 4 s in the file list: no cycle, no toast, linked: %s %d %s" % (side, dropped(v.run), v.cycles, v.texts))
    # SEAM-8: the OS offered the device (a match whose attach isn't done):
    # nothing cuts its power, no toast
    r = H.Run("sm_rv_offered").run(START + "pad preplug xinput\npad diag 1 0 0\nwait 14\nquit\n")
    v = View(r)
    H.check(v.cycles == 0 and not v.stray(), "SEAM-8: offered: no cycle, no toast: %d %s" % (v.cycles, v.texts))
    H.check(any("no power cycle yet: the OS offered the device to a driver" in l for l in v.lines), "SEAM-8: the log says why")
    # ...and a device the driver refused (unsupported): the port is its, no cycle, no last resort
    r = H.Run("sm_rv_refused").run(START + "pad preplug xinput\npad event unsupported xinput\nwait 14\nquit\n")
    v = View(r)
    H.check(v.cycles == 0 and v.holder() in ("OTHER", "WAIT") and LAST_RESORT not in v.texts and SWAP_CABLE not in v.texts,
            "SEAM-8: refused by the driver: no cycle, no last resort: %d %s %s" % (v.cycles, v.holders, v.texts))


def test_pad4_comeup():
    """PAD4: the come-up (the OS's time in one go at the plug), each way a
    pad comes up at the adapter's end, twice in one run with the adapter
    staying in (step 3: the second time the same as the first, nothing left
    from the one before), in a game with no spare time: the OS's host
    controller running (its hub's reset ~26 OS ticks: R10a's ~13 sleeps), the
    controller stopped until the OS has had 1000 of its ticks (R10a's 8BitDo,
    ~1060 sleeps), a device the port never latches (the restart at once).
    Port holders, cycles, toasts, the keys, and each plug's time to 'Controller
    connected'; then the menus (the file list) the same way."""
    shapes = (("sm_pad4_running", "slowhc ticks 0 26 xinput", 0, 1.0),
              ("sm_pad4_late", "slowhc ticks 1000 26 xinput", 0, 18.0),
              ("sm_pad4_noconn", "hcstop xinput", 1, 1.6))
    for name, dev, cyc, limit in shapes:
        sc = P.HEAVY + START + "pad port 1\nwait 1\npad cycle ok\n"
        for k in ("k1", "k2"):
            sc += "pad %s\nwait %g\n" % (dev, limit + 1.5) + hold_peek(k) + "release Pad_A\npad out\npad port 1\nwait 3\n"
        r = H.Run(name).run(sc + QUIET + "quit\n")
        v = View(r)
        v.dump(name)
        word = "pad " + dev.split()[0]
        plugs = v.time_of(word)
        ons = v.time_of("osd " + CONNECTED)
        took = []
        for i, a in enumerate(plugs):
            after = [b for b in ons if b >= a and (i + 1 >= len(plugs) or b < plugs[i + 1])]
            took.append(round(after[0] - a, 3) if after else None)
        H.check(len(took) == 2 and all(t is not None and t <= limit for t in took),
                "%s: each plug connected within %.1f s: %s" % (name, limit, took))
        H.check(len(took) == 2 and None not in took and abs(took[0] - took[1]) <= 0.5,
                "%s: the second plug as fast as the first (no state left): %s" % (name, took))
        H.check(v.firm == ["PAD", "PAD"] and v.holder() == "WAIT", "%s: holders PAD twice, WAIT between (the adapter stays): %s" % (name, v.holders))
        H.check(v.cycles == 2 * cyc, "%s: %d power restart(s) a plug: %d" % (name, cyc, v.cycles))
        H.check(v.texts == [COMEUP, CONNECTED, GONE] * 2, "%s: the come-up's message, the pad's, its goodbye, twice: %s" % (name, v.texts))
        H.check(keys(r, "k1") == "A" and keys(r, "k2") == "A", "%s: the pad's A reaches the game each time: %s %s" % (
            name, keys(r, "k1"), keys(r, "k2")))
        print("  PAD4 %s: plug to connected %s s" % (name, took))
    # the file list (the menus): the burst there at once, no message (no picture to put it on)
    for name, dev, cyc, limit in (("sm_pad4_menu_running", "slowhc ticks 0 26 xinput", 0, 1.0),
                                  ("sm_pad4_menu_noconn", "hcstop xinput", 1, 1.6)):
        r = H.Run(name).run("wait 1\npad cycle ok\npad %s\nwait %g\npress enter\nwait 2\nquit\n" % (dev, limit + 1))
        v = View(r)
        text = P.pad_file(r)
        att = P.step_ms(text, "the driver attached it")
        H.check(len(att) == 1 and att[0] <= limit * 1000, "%s: attached in the file list %s ms after the plug" % (name, att))
        H.check(v.cycles == cyc and "menus waited" in text, "%s: the burst in the menus, %d restart(s): %d" % (name, cyc, v.cycles))


TESTS = [test_matrix_empty, test_matrix_pad, test_matrix_calc, test_matrix_drive, test_swaps_extra, test_failure_rows, test_loops,
         test_docs, test_seam_cells, test_pad4_comeup]


def main():
    if not os.path.exists(P.USBPAD_H):
        print("seamless tests skipped: no %s (gpsp-host has no pads)" % os.path.normpath(P.USBPAD_H))
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
