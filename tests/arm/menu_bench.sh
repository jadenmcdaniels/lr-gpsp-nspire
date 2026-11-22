#!/bin/bash
# The menus' own CPU work on the calculator, step by step (tests/arm/menu_bench.c):
# runs it under qemu-arm with the ARM926 model as the CX II (396 MHz) and the
# CX (132 MHz, tests/calc_speed.py's cx timings) and prints each step's
# estimated milliseconds (the model's cycles times 1.05).
#   tests/arm/menu_bench.sh [STATE.svs.tns]
# STATE defaults to the reference Emerald slot 31 (a state with a picture). The
# file system's part of the same paths: tests/libbench.sh (menu-* paths).
set -eu
cd "$(dirname "$0")"
command -v nspire-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
export QEMU=${QEMU:-$(command -v qemu-arm)}
STATE=${1:-${EMERALD_STATES:?give a state, or set EMERALD_STATES}/Pokemon - Emerald Version (USA, Europe).gba31.svs.tns}
make -s build/menu_bench >/dev/null
# (the semihosted command line splits at spaces)
cp "$STATE" build/menu_bench_state.svs
python3 -I - build/menu_bench_state.svs <<'PY'
import os, subprocess, sys
sys.path.insert(0, os.path.abspath('..'))
import calc_speed
plugin = calc_speed.build()
syms = {n: a for a, _, n in calc_speed.symbols('build/menu_bench')}
rows = {}
names = None
for calc, (mhz, model_args, _) in (('cx2', calc_speed.CALCS['cx2']), ('cx', calc_speed.CALCS['cx'])):
    out = 'build/menu_bench.%s.model' % calc
    pa = '%s,mark=%x,out=%s' % (plugin, syms['perf_frame_mark'], out) + (',' + model_args if model_args else '')
    res = subprocess.run([os.environ['QEMU'], '-cpu', 'arm926', '-plugin', pa, 'build/menu_bench', sys.argv[1]],
                         check=True, capture_output=True, text=True)
    names = [l[5:] for l in res.stdout.split('\n') if l.startswith('step ')]
    frames = [int(l.split()[1]) for l in open(out) if l.startswith('frame')][1:]
    rows[calc] = [c * calc_speed.SCALE / (mhz * 1000.0) for c in frames]
print('%7s %7s  %s' % ('CX II', 'CX', 'step (ms, ARM926 model x %.2f)' % calc_speed.SCALE))
for i, name in enumerate(names):
    print('%7.1f %7.1f  %s' % (rows['cx2'][i], rows['cx'][i], name))
PY
