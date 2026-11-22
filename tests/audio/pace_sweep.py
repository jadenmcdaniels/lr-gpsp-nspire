#!/usr/bin/env python3
"""The pad's OS ticks under the anchored model (2026-11-17):

    python3 -I tests/audio/pace_sweep.py PACE_TEST [--usb8] [--usb-task MS]

PACE_TEST is tests/host/build-sink-pace/pace_test (tests/audio/unit_pace.sh builds it). Its pad case
runs each load with r15's rule as shipped (the glue's at the key read, the tick's phase from the
last wake, which comes the OS's own time after the tick) and with the frame loop's watch (the
tick's phase from its timer); this runs it over the OS tick 10.0 / 10.25 ms, the OS's own time at a
wake 0.1-2.5 ms and a jitter on it 0 / 0.5 / 1.0 ms, the USB task's time charged (a sleep serves the
pad only if it begins that long before its tick: 0.43 ms, R11u), and prints per case the watch's
gain over r15's (pictures; game frames in fast forward), its worst, every pair where the watch
is worse, the longest gaps (the OS's: the heartbeat's "OS gap"; the pad's served turns), and a
table at tick 10.25 / jitter 0.5. --usb8: final/usb8's rule composed (no pad tick of its own while
the sound streams). Exit 1 if the watch is worse anywhere (0.05 pictures, 0.5 game frames)."""
import itertools
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

exe = sys.argv[1]
extra = []
if '--usb8' in sys.argv[2:]:
    extra.append('--usb8')
task = '0.43'
if '--usb-task' in sys.argv[2:]:
    task = sys.argv[sys.argv.index('--usb-task') + 1]
ticks = (10.0, 10.25)
wakes = (0.1, 0.3, 0.6, 1.0, 1.6, 2.5)
jits = (0.0, 0.5, 1.0)


def run(args):
    """{(case, 'r15' | 'watch'): (fps, shown, os ms/s, os gap, pad gap)}"""
    out = subprocess.run([exe, '--case', 'pad', '--usb-task', task] + args + extra,
                         capture_output=True, text=True).stdout.splitlines()
    res = {}
    for i, l in enumerate(out):
        m = re.match(r'pace (\S+-pad\S*)\s+game\s+([\d.]+) fps, pictures\s+([\d.]+), LCD showed\s+([\d.]+).*?OS\s+([\d.]+) ms/s', l)
        if not m or i + 1 >= len(out):
            continue
        g = re.search(r'the longest without the OS ([\d.]+) ms; .*the longest between them ([\d.]+)', out[i + 1])
        who = 'r15' if "r15's rule" in out[i + 1] else 'watch'
        res[(m.group(1), who)] = (float(m.group(2)), float(m.group(4)), float(m.group(5)), float(g.group(1)), float(g.group(2)))
    return res


with ThreadPoolExecutor(12) as ex:
    jobs = {k: ex.submit(run, ['--tick', str(k[0]), '--lcd-wake', str(k[1]), '--jitter', str(k[2])])
            for k in itertools.product(ticks, wakes, jits)}
rows = []
for (t, w, j), f in jobs.items():
    res = f.result()
    for (case, who), a in res.items():
        if who == 'r15' and (case, 'watch') in res:
            rows.append((case, t, w, j, a, res[(case, 'watch')]))


def score(case, v):
    return v[0] if case.endswith('-ff') else v[1]


print("the watch (B) against r15's rule as shipped (A); the USB task %s ms%s" % (task, ', usb8 composed' if extra else ''))
print('%-26s | %-30s | %-17s | %-15s' % ('case', 'B - A: mean, worst (tick wake jitter)', 'OS gap max A / B', 'pad gap max A / B'))
worse = []
for c in sorted(set(r[0] for r in rows)):
    rr = [r for r in rows if r[0] == c]
    d = [score(c, r[5]) - score(c, r[4]) for r in rr]
    k = min(range(len(rr)), key=lambda i: d[i])
    print('%-26s | %+7.2f %+7.2f (%5.2f %3.1f %3.1f)  | %6.1f %6.1f     | %6.1f %6.1f' % (
        c, sum(d) / len(d), d[k], rr[k][1], rr[k][2], rr[k][3], max(r[4][3] for r in rr), max(r[5][3] for r in rr),
        max(r[4][4] for r in rr), max(r[5][4] for r in rr)))
    for i, r in enumerate(rr):
        if d[i] < (-0.5 if c.endswith('-ff') else -0.05):
            worse.append((r, d[i]))
print('pairs where the watch is worse: %d of %d' % (len(worse), len(rows)))
for (c, t, w, j, a, b), d in worse:
    print('  %-26s tick %.2f wake %.1f jitter %.1f: %+.2f (%.2f/%.2f -> %.2f/%.2f)' % (c, t, w, j, d, a[0], a[1], b[0], b[1]))
print('\ntick 10.25, jitter 0.5: case, wake | A game frames / pictures / OS ms a second | B')
for r in sorted(rows):
    if r[1] == 10.25 and r[3] == 0.5:
        print('%-26s %3.1f | %6.2f %5.2f %5.1f | %6.2f %5.2f %5.1f' % (r[0], r[2], r[4][0], r[4][1], r[4][2], r[5][0], r[5][1], r[5][2]))
sys.exit(1 if worse else 0)
