#!/bin/bash
# One scene on one calculator model, everything a speed hunt needs:
#   tests/speed.sh SCENE [cx2|cx|cx192] [more calc_speed.py arguments]
#   tests/speed.sh list          the scenes
# Prints the exact instructions a frame (no placement changes them: judge
# a small change by these), the model's ms for the game and a drawn frame
# as the mean and range of 8 placements of the dynarec's blocks and the
# stack (LAYOUTS=N for another count), the time by holder of the code and
# the 25 costliest functions; every function's line goes to
# tests/arm/build/speed/SCENE-CALC.txt. Models: cx2 (CX II, 396 MHz, the
# default), cx192 (the older CX at 192 MHz, its default now), cx (132 MHz).
# The model runs the calculator build's layout (gpsp_libretro.elf, copied
# modulo 4 KB by tests/arm/mirror_layout.py): make in the top folder after a
# change, then this (the "layout:" line says when the .elf is older than
# the sources). To compare two builds: copy tests/arm/build/perf away,
# change, rebuild both, and pass --elf COPY to one of the runs.
cd "$(dirname "$0")/.." || exit 1
scene=${1:?usage: tests/speed.sh SCENE [cx2|cx|cx192] [calc_speed.py arguments]}
shift
[ "$scene" = list ] && exec tests/calc_speed.sh --scene list
calc=cx2
case $1 in cx2|cx|cx192) calc=$1; shift ;; esac
exec tests/calc_speed.sh --scene "$scene" --calc "$calc" --layouts "${LAYOUTS:-8}" --regions --top 25 \
  --listing "tests/arm/build/speed/$scene-$calc.txt" --tag "$scene-$calc" "$@"
