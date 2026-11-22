#!/usr/bin/env python3
"""Reads gpsp_link.txt.tns (one or more logs) and tells what the seamless USB
port did (S10).

  seamless_log_check.py [-q] LOG [LOG...]

Part 1, for each plug event (a "seamless: port STATE" line): the state, the
stage that resolved it (offered by the OS / bus power cycle N / wait again /
direct read / gave up), who got the port, and the time from the port's change
to the toast ("Controller connected", "Linked: player N"), against the limits
3 s (pad) and 5 s (link). Part 2, the pad's own story: device offered (USB id,
class), name, attach time, first report, buttons seen, detach, and the failure
events (replug hint, power refusal, unsupported, "OS offered no device").
Exit 0 when every event with a toast is in its limit and nothing "gave up";
1 when not; 2 when a file can't be read. A log without seamless lines (an
older build) prints a note and exits 0. Never raises on odd lines.

Times in the log are ms from the start of the run (they restart at 0 when a
link session starts: a decrease starts a new segment; times are only compared
inside one segment)."""

import re
import sys

LIMIT_PAD, LIMIT_LINK = 3.0, 5.0
LINE = re.compile(r"^\s*(\d+) (.*)$")
STAGES = {0: "offered by the OS", 1: "bus power cycle", 2: "wait again", 3: "direct read", 4: "gave up"}


class Event:
    def __init__(self, seg, t, state, text):
        self.seg, self.t, self.state, self.text = seg, t, state, text
        self.change = None        # ms of the port's change (from "N ms after the port change"), else t
        self.stages = []          # (stage number, t)
        self.cycles = []          # t of each "reenum: cycle"
        self.refused = 0
        self.os_side = []         # "OS side" descriptions
        self.owner = None
        self.toasts = []          # (t, text)

    def t0(self):
        return self.change if self.change is not None else self.t

    def resolved_by(self):
        """The stage that resolved it: the last stage the port went through
        before the port had an owner; 'offered' when there was none."""
        if any(n == 4 for n, _ in self.stages):
            return "gave up"
        if self.owner in ("PAD", "LINK") and not self.stages:
            return "offered by the OS (no stage needed)"
        if self.owner in ("PAD", "LINK"):
            n = max(n for n, _ in self.stages)
            if self.cycles:
                return "bus power cycle %d" % len(self.cycles)
            return STAGES.get(n, "stage %d" % n)
        if self.stages:
            return "unresolved after stage %d (%s)" % (self.stages[-1][0], STAGES.get(self.stages[-1][0], "?"))
        return "-" if self_state_not_empty(self) else "(port empty)"


def self_state_not_empty(e):
    return e.state != "EMPTY"


def parse(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read().split("\n")
    except OSError as e:
        print("%s: %s" % (path, e))
        return None
    lines, seg, last = [], 0, -1
    for l in raw:
        m = LINE.match(l)
        if not m:
            if l.strip():
                lines.append((seg, None, l.strip()))
            continue
        t = int(m.group(1))
        if t < last:
            seg += 1
        last = t
        lines.append((seg, t, m.group(2)))
    return lines


def toast_kind(text):
    if text.startswith("Controller connected") or text.startswith("Controller 2"):
        return "pad"
    if text.startswith("Linked: player"):
        return "link"
    return None


def events_of(lines):
    evs, cur, early = [], None, None
    for seg, t, text in lines:
        if t is None:
            continue
        m = re.match(r"seamless: port (EMPTY|HOST_END|DEVICE_END|UNKNOWN) \(bits ([0-9a-f]+)\)", text)
        if m:
            cur = Event(seg, t, m.group(1), text)
            evs.append(cur)
            if early and early[0] == seg and t - early[1] <= 2000 and cur.state != "EMPTY":
                cur.owner = early[2]   # the OS offered it before the observer's next look
            early = None
            continue
        if cur is None or cur.seg != seg:
            m = re.match(r"seamless: port owner (PAD|LINK) \(was", text)
            if m:
                early = (seg, t, m.group(1))
            continue
        m = re.search(r"(\d+) ms after the port change", text)
        if m and text.startswith("seamless: ") and t >= int(m.group(1)):
            c = t - int(m.group(1))
            cur.change = c if cur.change is None else min(cur.change, c)
        m = re.match(r"seamless: stage (\d)", text)
        if m:
            cur.stages.append((int(m.group(1)), t))
        elif text.startswith("reenum: cycle"):
            cur.cycles.append(t)
        elif text.startswith("seamless: cycle") and "refused" in text:
            cur.refused += 1
        elif text.startswith("seamless: OS side:"):
            cur.os_side.append(text[len("seamless: OS side: "):].split(" ms after")[0].rsplit(" ", 1)[0])
        else:
            m = re.match(r"seamless: port owner (\w+) \(was", text)
            if m and m.group(1) in ("PAD", "LINK"):
                if cur.state == "EMPTY":
                    early = (seg, t, m.group(1))
                else:
                    cur.owner = m.group(1)
            elif text.startswith("toast: "):
                cur.toasts.append((t, text[7:]))
    return evs


def report_events(evs):
    bad = 0
    print("== Port events (%d) ==" % len(evs))
    for i, e in enumerate(evs, 1):
        print("%d. at %d ms: port %s" % (i, e.t, e.state))
        print("   resolved by: %s%s" % (e.resolved_by(), "; cycles %d" % len(e.cycles) if e.cycles else ""))
        if e.refused:
            print("   power cycle refused %d time(s) (the port's registers say it is unsafe)" % e.refused)
        if e.os_side:
            print("   OS side: %s" % "; ".join(e.os_side))
        if e.owner:
            print("   port owner: %s" % e.owner)
        for t, text in e.toasts:
            k = toast_kind(text)
            dt = (t - e.t0()) / 1000.0 if t >= e.t0() else None
            if k and dt is not None:
                lim = LIMIT_PAD if k == "pad" else LIMIT_LINK
                ok = dt <= lim
                bad += not ok
                print("   toast: %r after %.2f s (limit %.0f s): %s" % (text, dt, lim, "ok" if ok else "TOO SLOW"))
            else:
                print("   toast: %r" % text)
        if any(n == 4 for n, _ in e.stages):
            bad += 1
            print("   GAVE UP: nothing offered the device")
    return bad


def pad_story(lines):
    """The pad's own events, in order, with what the log says of each."""
    out, buttons, attached = [], {}, set()
    for seg, t, text in lines:
        if t is None or not text.startswith("pad: "):
            continue
        s = text[5:]
        m = re.match(r"event: (\w+)[^,]*, slot (\d+), ([0-9a-f]{4}):([0-9a-f]{4}), class (.*) \((\d+)\)", s)
        if m:
            kind, slot, vid, pid, name, cls = m.groups()
            if kind == "attach":
                buttons[slot] = []
                attached.add(slot)
                out.append((t, "device offered: %s:%s, class %s (kind %s), slot %s; attached at %d ms" % (vid, pid, name, cls, slot, t)))
            elif kind == "detach":
                b = buttons.get(slot)
                attached.discard(slot)
                out.append((t, "detach: slot %s at %d ms%s" % (slot, t, "; buttons seen: %s" % (", ".join(b) if b else "none") if b is not None else "")))
            else:
                out.append((t, "%s: %s:%s (%s) at %d ms" % (kind, vid, pid, name, t)))
            continue
        m = re.match(r"first report: slot (\d+), ([0-9a-f]{4}):([0-9a-f]{4})", s)
        if m:
            att = [x for x in out if x[1].startswith("device offered") and ("slot %s;" % m.group(1)) in x[1]]
            gap = " (%.2f s after the attach)" % ((t - att[-1][0]) / 1000.0) if att and t >= att[-1][0] else ""
            out.append((t, "first report: slot %s at %d ms%s" % (m.group(1), t, gap)))
            continue
        m = re.match(r"button: slot (\d+), (\w+)", s)
        if m:
            buttons.setdefault(m.group(1), []).append(m.group(2))
            continue
        if s.startswith("OS offered no device"):
            out.append((t, "no device offered by the OS: %s" % s))
        elif s.startswith("exit:"):
            out.append((t, "exit: %s" % s[5:].strip()))
    return out, buttons, attached


def report_pad(lines):
    out, buttons, attached = pad_story(lines)
    print("== The pad ==")
    if not out and not buttons:
        print("no pad lines in this log (no pad was offered, or the build has no gamepads)")
        return
    for t, text in out:
        print("  " + text)
    for slot, b in sorted(buttons.items()):
        if slot in attached:
            print("  buttons seen so far, slot %s (still attached): %s" % (slot, ", ".join(b) if b else "none"))


def report_comeup(lines):
    """DETECT (2026-11-11): each cable or device come-up: the message shown,
    the knock, the OS's node time, and the closing line with each step's ms
    after the port's change (or the pick)."""
    keys = ("come-up:", "NavNet: knock", "NavNet: a node listed", "NavNet: ready (", "NavNet: the other side connected")
    found = [(t, x) for _, t, x in lines if t is not None and x.startswith(keys)]
    if not found:
        return
    print("== Come-ups (plug-in to Linked) ==")
    for t, x in found:
        print("  %6d %s" % (t, x[:200]))


def check(path, quiet):
    lines = parse(path)
    if lines is None:
        return 2
    print("##### %s (%d lines)" % (path, len(lines)))
    if lines:
        print("first line: %s" % lines[0][2][:140])
    if not any(t is not None and text.startswith("seamless:") for _, t, text in lines):
        print("no 'seamless:' lines: a build without the seamless port, or nothing happened at the port")
        report_pad(lines)
        return 0
    evs = events_of(lines)
    bad = report_events(evs)
    report_comeup(lines)
    report_pad(lines)
    gave = sum(1 for _, t, x in lines if t is not None and "seamless: stage 4" in x)
    last = sum(1 for _, t, x in lines if t is not None and x == "toast: Unplug and replug the USB device")
    print("== Summary: %d port events, %d past their limit or gave up, 'gave up' stages %d, last-resort toasts %d ==" % (len(evs), bad, gave, last))
    return 1 if bad else 0


def main(argv):
    quiet = "-q" in argv
    paths = [a for a in argv if a != "-q"]
    if not paths:
        print(__doc__)
        return 2
    rc = 0
    for p in paths:
        rc = max(rc, check(p, quiet))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
