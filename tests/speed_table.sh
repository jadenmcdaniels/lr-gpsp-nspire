#!/bin/bash
# The speed hunt's baseline (speed hunt tooling): every named
# scene on every model through tests/speed.sh, one table line each:
#   scene calc | game ms (range over the layouts) | drawn frame ms | all ms
#   | exact instructions a frame: game, all | the frontend's summary
# Each run's whole output (time by holder, top functions) is kept in
# OUT/SCENE-CALC.out and every function in OUT/SCENE-CALC.txt.
#   tests/speed_table.sh [SCENE:CALC ...]     default: all of them (long)
#   LAYOUTS=N (default 8; 4 for the 2400-frame intros), JOBS=N runs at once
#   (default $GPSP_TEST_JOBS, or half the CPUs; each with its layouts one
#   after another, the longest first: the table keeps its order), OUT=folder
#   (default tests/arm/build/speed/baseline)
cd "$(dirname "$0")/.." || exit 1
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
out=${OUT:-tests/arm/build/speed/baseline}
mkdir -p "$out"
pairs=("$@")
if [ ${#pairs[@]} -eq 0 ]; then
  for c in cx192 cx cx2; do
    for s in dewford fallarbor walk wildwipe town trainwipe battle menu grass aw fe gs mc; do pairs+=("$s:$c"); done
  done
  pairs+=(ff:cx2 fftown:cx2 ffwalk:cx2)
fi
make -s -C tests/arm build/perf >/dev/null || exit 1
one() {   # scene:calc
  local s=${1%%:*} c=${1##*:} n=${LAYOUTS:-8}
  case $s in fe|gs|mc) n=${LAYOUTS:-4} ;; esac
  tests/speed.sh "$s" "$c" --layouts "$n" --jobs 1 --tag "bl-$s-$c" \
    --listing "$out/$s-$c.txt" > "$out/$s-$c.out" 2>&1 || echo "FAILED: $s $c (see $out/$s-$c.out)"
}
export -f one
export out LAYOUTS
# (one's text and these variables are in every run's environment, which the
# model's numbers can depend on: keep them as they are)
longest_first() {   # pairs, by their frames times layouts
  python3 -I -c 'import sys; sys.path.insert(0, "tests"); import calc_speed as c
pairs = sys.stdin.read().split()
def work(p):
    s = p.split(":")[0]
    return c.SCENES[s][3] * int(sys.argv[2] if s in ("fe", "gs", "mc") else sys.argv[1]) if s in c.SCENES else 0
print("\n".join(sorted(pairs, key=lambda p: -work(p))))' "${LAYOUTS:-8}" "${LAYOUTS:-4}"
}
printf '%s\n' "${pairs[@]}" | longest_first |
  xargs -P "${JOBS:-${GPSP_TEST_JOBS:-$(($(nproc) / 2))}}" -I{} bash -c 'one "$@"' _ {}
printf '%-10s %-5s | %-20s | %6s | %6s | %-19s | %s\n' scene calc "game ms (range)" drawn all \
  "instructions game/all" summary
for p in "${pairs[@]}"; do
  s=${p%%:*} c=${p##*:} f=$out/$s-$c.out
  [ -f "$f" ] || continue
  awk -v s="$s" -v c="$c" '
    $1 == "game" && !g {g = $2; r = $3}
    $1 == "drawing" && !d {d = $2}
    $1 == "picture" && !p {p = $2}
    $1 == "all" && !a {a = $2}
    /instructions a frame/ {ig = $7; sub(/,$/, "", ig); ia = $0; sub(/.* all /, "", ia); sub(/ .*/, "", ia)}
    /summary:/ {sub(/^ *summary: /, ""); sm = $0}
    END {printf "%-10s %-5s | %-20s | %6.2f | %6s | %-19s | %s\n", s, c, g " " r, g + d + p, a,
                ig "/" ia, sm}' "$f"
done
