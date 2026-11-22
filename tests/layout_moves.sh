#!/bin/bash
# How far a pure layout move changes the speed model (the model
# runs the calculator's layout). For each code pad (tN, N bytes in hex) the
# calculator build is linked again with tests/arm/layout_pad.S first
# (gpsp_libretro.elf's own objects, nothing recompiled) and the model copies
# that layout (tests/arm mirror_layout.py); a data pad (bN) the same way
# (N bytes of .bss before LTO's: its .bss moves, arm_stub.S's 4 KB-aligned
# GBA memory and caches don't; the model copies the data's places too,
# tests/arm/Makefile MIRROR_DATA=1). Each scene runs at LAYOUTS
# placements of the dynarec's blocks and the stack (default 4). One line per
# scene and pad: game ms and drawn-frame ms (mean over the placements) and
# their change against no pad.
#   tests/layout_moves.sh [PAD...]            default: t200 t600 t1000 b200 b600 b1000
#   SCENES="dewford:cx192 ff:cx2" tests/layout_moves.sh t600
#   RELINK=0 ...: reuse the padded builds of the last run; RERUN=0 ...: only
#   the table, from the last run's outputs
#   OUT=tests/arm/build/NAME: another folder for the outputs; JOBS=N runs and
#   LINKJOBS=N links at once (default $GPSP_TEST_JOBS, or half the CPUs)
# Needs a built calculator tree (make in the top folder). Outputs in
# tests/arm/build/moves/ (padded ELFs, models, every run's output).
cd "$(dirname "$0")/.." || exit 1
command -v nspire-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
pads=("$@")
[ ${#pads[@]} -eq 0 ] && pads=(t200 t600 t1000 b200 b600 b1000)
SCENES=${SCENES:-"dewford:cx192 dewford:cx ff:cx2 fftown:cx2 wildwipe:cx2 wildwipe:cx192 walk:cx2 town:cx2 trainwipe:cx2 battle:cx2 menu:cx2 aw:cx2"}
LAYOUTS=${LAYOUTS:-4}
JOBS=${JOBS:-${GPSP_TEST_JOBS:-$(($(nproc) / 2))}}
out=${OUT:-tests/arm/build/moves}
case $out in tests/arm/build/*) ;; *) echo "OUT must be in tests/arm/build/"; exit 1 ;; esac
rel=${out#tests/arm/}
mkdir -p "$out"
[ -f gpsp_libretro.elf ] || { echo "no gpsp_libretro.elf: build the calculator first"; exit 1; }
# The calculator's link, as the Makefile runs it.
cmd=$(make -n -B gpsp_libretro.elf 2>/dev/null | grep -- '-o gpsp_libretro.elf' | tail -1)
[ -n "$cmd" ] || { echo "can't find the calculator's link command"; exit 1; }
make -s -C tests/arm build/perf >/dev/null || exit 1

link_calc() {   # pad: tN (code) or bN (data), made in the calculator build; the model copies it
  local pad=$1 t=0 b=0
  case $pad in t*) t=0x${pad#t} ;; b*) b=0x${pad#b} ;; none) ;; *) echo "bad pad $pad"; return 1 ;; esac
  nspire-gcc -DPAD_TEXT=$t -DPAD_BSS=$b -c tests/arm/layout_pad.S -o "$out/pad-$pad.o" || return 1
  local c=${cmd/ -o gpsp_libretro.elf/ -o $out/calc-$pad.elf}
  # -u: --gc-sections would drop the pad (nothing uses it). First: LTO's
  # code and data come where the first IR object is, so all of it moves.
  c=${c/ compat_posix_string.o/ -Wl,-u,layout_pad_text -Wl,-u,layout_pad_bss $out/pad-$pad.o compat_posix_string.o}
  case $c in *"$out/pad-$pad.o"*"-o $out/calc-$pad.elf"*) ;; *) echo "can't place the pad in: $cmd"; return 1 ;; esac
  eval "$c" 2>&1 | grep -v lto-wrapper
  make -s -C tests/arm build/perf-from FROM="../../$out/calc-$pad.elf" OUT="$rel/perf-$pad" \
    2>&1 | grep -v lto-wrapper | grep -v '^mirror_layout: [0-9]* sections'
}
run() {   # pad scene calc
  local elf=$out/perf-$1
  tests/calc_speed.sh --scene "$2" --calc "$3" --layouts "$LAYOUTS" --jobs 1 --elf "$elf" \
    --tag "mv-$1-$2-$3" > "$out/$1-$2-$3.out" 2>&1
}
# The links, LINKJOBS at a time (each is a whole LTO link), each one's
# lines kept and said in order.
for pad in none "${pads[@]}"; do
  rm -f "$out/link-$pad.log"
  # RELINK=0: keep the models a run before built (the same gpsp_libretro.elf).
  [ "${RELINK:-1}" = 0 ] && [ -x "$out/perf-$pad" ] && continue
  while [ "$(jobs -rp | wc -l)" -ge "${LINKJOBS:-$JOBS}" ]; do wait -n; done
  { echo "== $pad"; link_calc "$pad" || echo "FAILED: $pad"; } > "$out/link-$pad.log" 2>&1 &
done
wait
for pad in none "${pads[@]}"; do
  if [ -f "$out/link-$pad.log" ]; then cat "$out/link-$pad.log"; rm "$out/link-$pad.log"; fi
done
for pad in none "${pads[@]}"; do [ -x "$out/perf-$pad" ] || { echo "no model for $pad"; exit 1; }; done
# The runs, JOBS at a time (each one runs its placements one after another).
jobs=()
for sc in $SCENES; do
  for pad in none "${pads[@]}"; do jobs+=("$pad ${sc%%:*} ${sc##*:}"); done
done
[ "${RERUN:-1}" = 0 ] || printf '%s\n' "${jobs[@]}" | xargs -P "$JOBS" -L 1 bash -c 'cd "'"$PWD"'" && '"$(declare -f run); out=$out; LAYOUTS=$LAYOUTS;"' run "$@"' _
val() {   # file -> "game drawn all"
  awk '$1 == "game" && !g {g = $2} $1 == "drawing" && !d {d = $2} $1 == "picture" && !p {p = $2}
       $1 == "all" && !a {a = $2} END {printf "%s %.2f %s", g, g + d + p, a}' "$1"
}
printf '%-10s %-6s %-6s %13s %13s %13s\n' scene calc pad "game ms" "drawn ms" "all ms"
for sc in $SCENES; do
  s=${sc%%:*} c=${sc##*:}
  read -r g0 d0 a0 < <(val "$out/none-$s-$c.out")
  for pad in none "${pads[@]}"; do
    read -r g d a < <(val "$out/$pad-$s-$c.out")
    awk -v s="$s" -v c="$c" -v p="$pad" -v g="$g" -v d="$d" -v a="$a" -v g0="$g0" -v d0="$d0" -v a0="$a0" \
      'BEGIN {printf "%-10s %-6s %-6s %6.2f %+5.1f%% %6.2f %+5.1f%% %6.2f %+5.1f%%\n", s, c, p, g, 100 * (g / g0 - 1),
              d, 100 * (d / d0 - 1), a, 100 * (a / a0 - 1)}'
  done
done
