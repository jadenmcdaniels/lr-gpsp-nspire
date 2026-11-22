#!/bin/bash
# Pokemon Emerald's scenes on both calculators (tests/calc_speed.sh's model),
# one line each: the game's ms a frame, a drawn frame's ms, and its summary
# (normal speed, fast forward). The scenes are calc_speed.py's (--scene
# list): the reference states read where they are ($EMERALD_STATES, default
# the newest *-emerald-states backup: cx2/ and, for
# Dewford, cx/), the route-to-grass replay from $GBA_GAMES's
# (ff: fast forward in Fallarbor at 180 with 60 pictures, CX II only)
# emerald_route_grass.svs.tns, and the effect scenes (rain storm sand ash surf
# dive fog: tests/emerald_fx_states.py's states in $GBA_GAMES; FX_SCENES=
# leaves them out). LAYOUTS=N: the mean of N placements.
# JOBS=N runs at once (default $GPSP_TEST_JOBS, or half the CPUs; the model's
# counts don't depend on it), the lines in the same order.
#   tests/emerald_scenes.sh [--calc cx2|cx|cx192|both] [SETTING=VALUE...]
cd "$(dirname "$0")/.." || exit 1
calcs="cx2 cx"
extra=()
[ -n "$LAYOUTS" ] && extra+=(--layouts "$LAYOUTS" --jobs 1)
while [ $# -gt 0 ]; do
  case $1 in
    --calc) [ "$2" = both ] || calcs=$2; shift 2 ;;
    *) extra+=(--set "$1"); shift ;;
  esac
done
tag=em$$
FX_SCENES=${FX_SCENES-rain storm sand ash surf dive fog}
run() {   # scene calc
  local out
  out=$(tests/calc_speed.sh --scene "$1" --calc "$2" --tag "$tag-$1-$2" "${extra[@]}" 2>&1)
  case $out in missing*|perf\ failed*) echo "$1: $out"; return ;; esac
  printf '%-10s %-5s game %6s  drawn %6s %s\n' "$1" "$2" \
    "$(echo "$out" | awk '$1 == "game" {print $2; exit}')" \
    "$(echo "$out" | awk '$1 == "drawing" {d = $2} $1 == "picture" {p = $2} $1 == "game" && !g {g = $2} END {printf "%.2f", g + d + p}')" \
    "$(echo "$out" | sed -n 's/^  summary: /| /p')"
}
tests/calc_speed.sh --help >/dev/null 2>&1
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
make -s -C tests/arm build/perf >/dev/null || exit 1
# JOBS at a time, each one's line kept and printed in this order.
njobs=${JOBS:-${GPSP_TEST_JOBS:-$(($(nproc) / 2))}}
lines=$(mktemp -d)
n=0
start() {   # scene calc
  while [ "$(jobs -rp | wc -l)" -ge "$njobs" ]; do wait -n; done
  run "$1" "$2" > "$lines/$n" &
  n=$((n + 1))
}
for c in $calcs; do
  for scene in walk wildwipe town trainwipe battle menu dewford fallarbor $FX_SCENES; do
    start $scene "$c"
  done
  # Fast forward with 60 pictures a second (one frame in 3 drawn): the CX II's.
  [ "$c" = cx2 ] && start ff "$c"
done
wait
for ((i = 0; i < n; i++)); do cat "$lines/$i"; done
rm -rf "$lines"
rm -f tests/arm/build/speed/$tag-*
