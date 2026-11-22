#!/bin/bash
# Exhaustive check of the integer frequency steps against the float formulas
# (steps_test.c), both timer bases. Run by run.sh (4).
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/steps.XXXXXX")
trap 'rm -rf "$T"' EXIT
for def in "" -DOVERCLOCK_60FPS; do
  gcc -O0 -std=gnu99 -msse2 -mfpmath=sse -ffp-contract=off $def "$D/steps_test.c" -o "$T/st" || exit 1
  "$T/st" >"$T/out" || { cat "$T/out"; exit 1; }
  cat "$T/out"
done
tail -1 "$T/out"
