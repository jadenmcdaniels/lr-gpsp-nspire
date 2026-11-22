#!/usr/bin/env python3
"""The link tests, and the link's speed: two gpsp-hosts linked by a socket
(tests/host/host_link.c) play a game together, each as its calculator
(linklib.MODELS: a CX II or an original CX, their costs and settings), the
link's messages taking a given time to come. The scenarios are in
tests/link/scenarios/*.py (linklib.Scenario: the game, each player's files
and script, a check); their scripts count frames, so they play the same at
any link delay.

    python3 -I tests/link/run.py [NAME...]           the quick ones (or these), no delay
                                                     (NAME can be a scenarios/ file's: compat)
    python3 -I tests/link/run.py --delays 0,3 NAME   NAME at each delay (ms, one way)
    python3 -I tests/link/run.py --matrix            every scenario at its matrix delays
    python3 -I tests/link/run.py --list              the scenarios
    -j N   runs at once (default half the CPUs: a run is two gpsp-hosts)

Each run prints its check and, for each player, its calculator and, while
linked: game frames a second (average, and the worst second), virtual
seconds, cable transfers, messages sent/received, and the virtual time
waiting on the link and the messages' cost. --matrix's table also goes to
tests/link/work/matrix.txt. A scenario is skipped without its files ($GBA_GAMES,
and $EMERALD_SAVE, a battery save of Emerald). Each run's
pictures and reports: tests/link/work/NAME@DELAY/p1|p2/out."""
import argparse, glob, importlib.util, os, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import linklib


def load_scenarios():
    """The scenarios by name, and each file's (scenarios/NAME.py: NAME) names."""
    found, groups = {}, {}
    for path in sorted(glob.glob(os.path.join(HERE, "scenarios", "*.py"))):
        stem = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location("link_scenarios_" + stem, path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for s in module.scenarios():
            found[s.name] = s
            groups.setdefault(stem, []).append(s.name)
    return found, groups


COLUMNS = "%-22s %5s  %-6s %-4s %6s %5s %8s %9s %15s %8s %7s  %s"
HEADER = COLUMNS % ("scenario", "delay", "player", "calc", "fps", "worst", "seconds", "transfers",
                    "sent/received", "wait s", "cost s", "result")


def rows(result):
    """The table's lines for a run: one a player."""
    run, ok, _ = result
    out = []
    for p in run.players:
        r = p.report
        out.append(COLUMNS % (run.scenario.name, "%gms" % run.delay, "p%d" % p.number,
                              run.scenario.models[p.number - 1], r.get("link_fps", "-"),
                              r.get("link_worst_fps", "-"), r.get("link_seconds", "-"),
                              r.get("link_transfers", "-"),
                              "%s/%s" % (r.get("link_messages_sent", "-"), r.get("link_messages_received", "-")),
                              r.get("link_wait", "-"), r.get("link_cost", "-"), "ok" if ok else "FAIL"))
    return out


def play(job):
    scenario, delay = job
    run = linklib.run(scenario, delay)
    try:
        ok, what = scenario.check(run)
    except Exception as e:   # what it reads isn't there: the run went wrong
        ok, what = False, "check: %r" % e
    for p in run.players:
        # R5: a packet handed to a stand-in off a word crashes a calculator.
        if p.report.get("link_unaligned", "0") != "0":
            ok = False
            what += " (player %d: %s packets delivered unaligned)" % (p.number, p.report["link_unaligned"])
        # R6: a key left in the OS's queue at exit reaches the file browser.
        if p.report.get("os_keys_left", "0") != "0":
            ok = False
            what += " (player %d: %s keys left in the OS's queue)" % (p.number, p.report["os_keys_left"])
        # EXIT-1: every exit ends with the hand-back, once: the OS's keypad
        # line on, nothing letting the OS run after it.
        # EXIT-R3: a report without these fields fails too. (Not the legacy
        # player: e24876d's gpsp-host predates the hand-back and has no such
        # fields; --matrix's compat runs play it.)
        if not p.legacy and (p.report.get("hand_backs") != "1" or p.report.get("os_keypad_irq_end") != "on" or
                p.report.get("os_after_hand_back") != "0"):
            ok = False
            what += " (player %d: hand-backs %s, keypad line %s, OS ran %s times after)" % (
                p.number, p.report.get("hand_backs"), p.report.get("os_keypad_irq_end"),
                p.report.get("os_after_hand_back"))
        if p.exit != 0 or p.stderr.strip():
            ok = False
            what += " (player %d: exit %d %s)" % (p.number, p.exit, p.stderr.strip()[:200])
    return run, ok, what


def main():
    parser = argparse.ArgumentParser(description="The link tests and the link's speed matrix.")
    parser.add_argument("names", nargs="*")
    parser.add_argument("--delays", default="0", help="link delays in ms, one way: 0,1,3,10")
    parser.add_argument("--matrix", action="store_true", help="every scenario at its matrix delays")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = parser.parse_args()

    scenarios, groups = load_scenarios()
    # A file's name stands for its scenarios (compat: scenarios/compat.py's).
    args.names = [n for name in args.names for n in (groups[name] if name in groups and name not in scenarios
                                                     else [name])]
    if args.list:
        for s in scenarios.values():
            legacy = ", e24876d as player %d" % (s.hosts.index("legacy") + 1) if "legacy" in s.hosts else ""
            print("%-22s %s, %s on %s/%s%s%s%s" % (s.name, " / ".join(dict.fromkeys(s.roms)), "/".join(s.devices),
                                                  s.models[0], s.models[1], legacy,
                                                ", matrix %s ms" % ",".join("%g" % d for d in s.delays)
                                                if s.delays else "", ", quick" if s.quick else ""))
        return 0
    unknown = [n for n in args.names if n not in scenarios]
    if unknown:
        print("no such scenario: %s (--list)" % " ".join(unknown))
        return 2
    delays = [float(d) for d in args.delays.split(",")]
    if args.matrix:
        jobs = [(s, d) for s in scenarios.values() if not args.names or s.name in args.names for d in s.delays]
    else:
        chosen = [scenarios[n] for n in args.names] or [s for s in scenarios.values() if s.quick]
        jobs = [(s, d) for s in chosen for d in delays]
    skipped = sorted({s.name for s, _ in jobs if s.missing()})
    for name in skipped:
        print("%-22s skipped: no %s" % (name, ", ".join(scenarios[name].missing())))
    jobs = [j for j in jobs if j[0].name not in skipped]

    results = []
    with ThreadPoolExecutor(max_workers=args.j) as pool:
        for result in pool.map(play, jobs):
            run, ok, what = result
            print("%-22s %s  %s" % ("%s@%gms" % (run.scenario.name, run.delay), "ok  " if ok else "FAIL", what))
            for line in rows(result):
                print("    " + line[23:])
            sys.stdout.flush()
            results.append(result)

    if args.matrix and results:
        table = [HEADER] + [line for result in results for line in rows(result)]
        # The games play the same at every delay: so player 1's transfers
        # and what it sends, and both players' words at their cycles (the
        # cable's link_transfer_sum).
        notes = []
        for name in dict.fromkeys(r[0].scenario.name for r in results):
            counts = {r[0].delay: (r[0].players[0].report.get("link_transfers"),
                                   r[0].players[0].report.get("link_bytes_sent"),
                                   r[0].players[0].report.get("link_transfer_sum"),
                                   r[0].players[1].report.get("link_transfer_sum"))
                      for r in results if r[0].scenario.name == name}
            if len(counts) > 1:
                same = len(set(counts.values())) == 1
                notes.append("%s: %s transfers, words and bytes sent at every delay%s" % (
                    name, "the same" if same else "NOT the same", "" if same else " %s" % counts))
        text = "\n".join(table + [""] + notes) + "\n"
        os.makedirs(linklib.WORK, exist_ok=True)
        with open(os.path.join(linklib.WORK, "matrix.txt"), "w") as f:
            f.write(text)
        print()
        print(text, end="")
    passed = sum(1 for r in results if r[1])
    print("%d of %d link runs passed%s" % (passed, len(results), ", %d skipped" % len(skipped) if skipped else ""))
    return 0 if passed == len(results) else 1


sys.exit(main())
