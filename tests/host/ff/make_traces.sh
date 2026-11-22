#!/bin/bash
# The speed model's frame costs for fast forward's rhythm tests
# (host_tests.py test_fast_forward_even, ff_rhythm.py): each Emerald scene of
# tests/emerald_scenes.sh on each calculator, one line a frame (game,
# drawing, picture ms), that gpsp-host's "cost trace FILE" plays back.
#   tests/host/ff/make_traces.sh      (writes tests/host/ff/SCENE-CALC.txt)
# Needs what tests/emerald_scenes.sh needs (the reference states, the model).
cd "$(dirname "$0")/../../.." || exit 1
G=${GBA_GAMES:-}
ST=${EMERALD_STATES:-}/cx2
ROM="$G/Pokemon - Emerald Version (USA, Europe).gba"
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
make -s -C tests/arm build/perf >/dev/null || exit 1
s() { echo "$ST/Pokemon - Emerald Version (USA, Europe).gba$1.svs.tns"; }
one() {   # scene calc state keys frames skip
  tests/calc_speed.sh "$3" --rom "$ROM" --keys "$4" --frames "$5" --skip "$6" --calc "$2" \
    --tag "fftrace-$1-$2" --trace "tests/host/ff/$1-$2.txt" | sed -n 's/^  summary: /'"$1 $2"': /p'
}
for c in cx2 cx192 cx; do
  one walk "$c" "$G/emerald_route_grass.svs.tns" tests/arm/replay/emerald_grass.keys 290 30
  one wipe "$c" "$G/emerald_route_grass.svs.tns" tests/arm/replay/emerald_grass.keys 345 298
  one town "$c" "$(s 24)" - 300 30
  one battle "$c" "$(s 30)" - 300 30
done
rm -f tests/arm/build/speed/fftrace-*
