#!/bin/bash
# Builds and runs the ARM tests (tests/arm/Makefile) under qemu-arm.
# Needs the Ndless toolchain (on PATH, or NSPIRE_ENV) and qemu-arm (QEMU=,
# by default the one on PATH).
#
# The Pokemon Emerald replay needs files that can't be in the repository,
# in $GBA_GAMES (no default):
#   Pokemon - Emerald Version (USA, Europe).gba   the game (.zip: also tried)
#   gba_bios.bin                                  the GBA BIOS
#   emerald_route_grass.svs.tns                   a gpSP save state on a
#                                                 route next to tall grass
#   Advance Wars (USA) (Rev 1) (damaged).gba      the reference bad dump (CRC32
#                                                 418740D0, not No-Intro's)
# Without them those are skipped (said so).
# GPSP_TEST_JOBS=N (default half the CPUs): the builds and the runs N at a
# time, every result said in this file's order; 1: one run at a time.
set -u
cd "$(dirname "$0")"
QEMU=${QEMU:-$(command -v qemu-arm)}
GBA_GAMES=${GBA_GAMES:-}
[ -x "$QEMU" ] || { echo "no qemu-arm at $QEMU"; exit 1; }
command -v nspire-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
JOBS=${GPSP_TEST_JOBS:-$(($(nproc) / 2))}
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
# The tests and the check builds (below), all built side by side (builds.mk: one make's jobs; a build that
# fails is said in the results, the main one stops the run).
mkdir -p build
make -s -f builds.mk -j"$JOBS" >/dev/null 2>&1
built() { [ "$(cat "build/built-$1.rc" 2>/dev/null)" = 0 ]; }   # NAME: that build made
built main || { cat build/built-main.log 2>/dev/null; echo "build failed"; exit 1; }
# The PC word check build (core-17's oracle, PCWORD_CHECK=1), for its run below.
built pcword; pcword_built=$?
# The idle check build (core-16's and speed9b's oracle, IDLE_CHECK=1), for
# Advance Wars' idle pass below.
built idlecheck; idlecheck_built=$?
# The line check build (speed10's oracle, LINE_CHECK=1: every line kept as
# it was is drawn too and compared), for the batched drawing cases below.
built linecheck; linecheck_built=$?
# speed10b's kept RAM blocks (cpu_threaded.c): the check build (KEEP_CHECK=1:
# every copy of a kept block made as on the calculator, undone, the block
# translated instead and compared byte for byte; every flush predicted
# instead of a copy checked to happen), also with a small PC table and ROM
# cache (RAMPCS=1, rampcs=N, ROMCACHE_KB=256: PC table flushes next to
# copies, ROM flushes that empty the store); and trace builds with the
# blocks kept and without (KEEP_TRACE=1, NOKEEP=1: every block made, the RAM
# cache's code, tables and tags as each frame ends) at full size, in 16 and
# 32 KB RAM caches (RAMCACHE_KB: the store reached by the code and the tag
# table often) and with the small PC table and ROM cache. dynarec_test's
# ROM in the check and the full-size trace builds too (its BIOS calls from
# IWRAM: a copy's branch into the ROM cache).
# (built above, by builds.mk)
keep_built=0
for b in keepcheck keepcheckx ktr ktrn ktr16 ktr16n ktr32 ktr32n ktrx ktrxn; do
  built $b || keep_built=1
done

failed=0

# Every run starts first, JOBS at a time, the longest first (start ID
# COMMAND...: its output and exit status kept in build/jobs); then each one's
# result is said in this file's order (finish ID: waited for, its exit status
# in $status).
rm -rf build/jobs
mkdir -p build/jobs
start() {
  local id=$1
  shift
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  ( t0=$(date +%s.%N); "$@" > "build/jobs/$id.out" 2>&1; echo $? > "build/jobs/$id.tmp"
    awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f\n", b - a }' > "build/jobs/$id.secs"
    mv "build/jobs/$id.tmp" "build/jobs/$id.rc" ) &
}
finish() {
  while [ ! -f "build/jobs/$1.rc" ]; do wait -n 2>/dev/null || sleep 0.05; done
  status=$(cat "build/jobs/$1.rc")
}

# A test program; a crash fails too (qemu stops at an unaligned stmia as the
# calculator does). start_run ID ARGS...: started; run ID NAME: its result.
start_run() {
  local id=$1
  shift
  start "$id" "$QEMU" -cpu arm926 "$@"
}
run() {
  local name=$2
  finish "$1"
  cat "build/jobs/$1.out"
  if [ $status -gt 128 ]; then
    echo "FAIL: $name crashed (signal $((status - 128)))"
  fi
  [ $status -eq 0 ] || failed=$((failed + 1))
}

# ---- The runs, started (their results below) ----

# Batched drawing's cases (below): name, game, state (- for power-on), keys
# (-: none), frames, draw (1 every frame, 2 every other one).
batch_cases=(
  "Advance Wars, the reference name entry|Advance Wars (USA) (Rev 1).gba|aw_latest.svs.tns|-|600|1"
  "Advance Wars, the reference name entry, every other frame|Advance Wars (USA) (Rev 1).gba|aw_latest.svs.tns|-|600|2"
  "Advance Wars from power-on|Advance Wars (USA) (Rev 1).gba|-|replay/boot.keys|2400|1"
  "Emerald into the tall grass|Pokemon - Emerald Version (USA, Europe).gba|emerald_route_grass.svs.tns|replay/emerald_grass.keys|1900|1"
  "Emerald from power-on|Pokemon - Emerald Version (USA, Europe).gba|-|replay/boot.keys|2400|1"
  "Fire Emblem from power-on|Fire Emblem - The Sacred Stones (USA, Australia).gba|-|replay/boot.keys|2400|1"
  "Golden Sun from power-on|Golden Sun (USA, Europe).gba|-|replay/boot.keys|2400|1"
  "Minish Cap from power-on|Legend of Zelda, The - The Minish Cap (USA).gba|-|replay/boot.keys|2400|1"
  # The effect scenes (tests/emerald_fx_states.py; their frames stored from
  # 7baa00e, before the r19-fx renderer changes): ST objects at 16/0 (sand,
  # ash) and blended (fog, underwater), rain sprites, lightning, surfing.
  "Emerald in the rain on Route 119|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_rain.svs.tns|replay/fx_rain.keys|330|1"
  "Emerald in the thunderstorm on Route 119|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_storm.svs.tns|replay/fx_rain.keys|330|1"
  "Emerald in the sandstorm on Route 111|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_sand.svs.tns|replay/fx_sand.keys|330|1"
  "Emerald in the ash on Route 113|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_ash.svs.tns|replay/fx_ash.keys|330|1"
  "Emerald surfing on Route 124|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_surf.svs.tns|replay/fx_surf.keys|330|1"
  "Emerald diving on Underwater Route 124|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_dive.svs.tns|replay/fx_dive.keys|330|1"
  "Emerald in the fog on Mt Pyre|Pokemon - Emerald Version (USA, Europe).gba|emerald_fx_fog.svs.tns|replay/fx_fog.keys|330|1"
)
batch_one() {   # index batch [settings] (the links are made beforehand; settings: the output's name gets -w)
  local IFS='|' f out perf=build/perf
  read -r _ game state keys frames draw <<< "${batch_cases[$1]}"
  f=build/batch/c$1
  out=$f-b$2$([ $# -gt 2 ] && echo -w).txt
  [ "$2" = c ] && { perf=build/linecheck/perf; out=$f-c.txt; set -- "$1" 1; }
  IFS=' '
  "$QEMU" -cpu arm926 $perf $f.gba build/games/gba_bios.bin "$([ "$state" = - ] && echo - || echo $f.svs)" \
    "$frames" "$keys" "$draw" batch=$2 every=1 "${@:3}" > $out 2>&1 || echo "exit $?" >> $out
}
batch_ready() {   # index: the case's game (and state) there
  local name game state
  IFS='|' read -r name game state _ <<< "${batch_cases[$1]}"
  [ -f "$GBA_GAMES/$game" ] && { [ "$state" = - ] || [ -f "$GBA_GAMES/$state" ]; }
}
if [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  mkdir -p build/batch build/games
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  # (the most frames first)
  for i in $(for i in "${!batch_cases[@]}"; do
               IFS='|' read -r _ _ _ _ frames _ <<< "${batch_cases[$i]}"; echo "$frames $i"; done | sort -k1,1nr -k2,2n | cut -d' ' -f2); do
    batch_ready "$i" || continue
    IFS='|' read -r _ game state _ <<< "${batch_cases[$i]}"
    ln -sf "$GBA_GAMES/$game" build/batch/c$i.gba
    [ "$state" = - ] || ln -sf "$GBA_GAMES/$state" build/batch/c$i.svs
    for b in 0 1; do start "batch$i-$b" batch_one "$i" "$b"; done
    start "batch$i-w" batch_one "$i" 1 oswatch=1 osclobber=1
    [ $linecheck_built = 0 ] && start "batch$i-c" batch_one "$i" c
  done
fi

EMERALD_FRAMES=replay/emerald_grass.frames
EMERALD_LAST=$(tail -1 $EMERALD_FRAMES)
emerald="$GBA_GAMES/Pokemon - Emerald Version (USA, Europe).gba"
emerald_replays=0
if [ -f "$emerald" ] && [ -f "$GBA_GAMES/gba_bios.bin" ] && [ -f "$GBA_GAMES/emerald_route_grass.svs.tns" ]; then
  emerald_replays=1
  # Semihosting splits the command line at spaces: links without them.
  mkdir -p build/games build/replays
  ln -sf "$emerald" build/games/emerald.gba
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  ln -sf "$GBA_GAMES/emerald_route_grass.svs.tns" build/games/emerald_route_grass.svs
  roms="emerald.gba"
  if [ -f "${emerald%.gba}.zip" ]; then
    ln -sf "${emerald%.gba}.zip" build/games/emerald.zip
    roms="$roms emerald.zip"
  fi
fi
each_replay() {   # COMMAND: COMMAND ROM POLICY ROMBUF for each setting
  local rom policy rombuf
  for rom in $roms; do
    for policy in 0 1; do
      for rombuf in 8 32; do
        [ "$rom" = emerald.zip ] && [ $rombuf = 32 ] && continue   # zips are unpacked whole anyway
        "$1" "$rom" "$policy" "$rombuf"
      done
    done
  done
}
start_replay() {
  start "replay-$1-p$2-b$3" "$QEMU" -cpu arm926 build/replay build/games/$1 build/games/gba_bios.bin \
    build/games/emerald_route_grass.svs $3 $2 1900 replay/emerald_grass.keys
}
[ $emerald_replays = 0 ] || each_replay start_replay

# Pokemon Emerald's cartridge clock (rtc_test.c): fresh power-on (auto/on/off,
# CX II and CX 1970 clocks, .gba and .gbz with 2 MB ROM cache), New Game
# followed by Start over, and an older state (rtc-status=2) followed by Start over.
emerald_rtc=0
if [ -f "$emerald" ] && [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  emerald_rtc=1
  mkdir -p build/games
  ln -sf "$emerald" build/games/emerald.gba
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  if [ ! -f build/games/emerald.gbz.tns ] || [ ../../tools/gbz.py -nt build/games/emerald.gbz.tns ]; then
    python3 -I ../../tools/gbz.py pack -o build/games build/games/emerald.gba >/dev/null
  fi
  start rtc_newgame "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin - on 32 7000 1795091696 8000 replay/emerald_newgame.keys
  start rtc_auto_cx2 "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin - auto 32 0 1795091696 1000 -
  start rtc_on_cx2 "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin - on 32 0 1795091696 1000 -
  start rtc_on_cx "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin - on 32 0 6152 1000 -
  start rtc_off "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin - off 32 0 1795091696 1000 -
  start rtc_gbz_rb2 "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gbz.tns build/games/gba_bios.bin - on 2 0 1795091696 1000 -
  if [ $emerald_replays = 1 ]; then
    start rtc_state_auto "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin build/games/emerald_route_grass.svs auto 32 1 1795091696 1000 -
    start rtc_state_on "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gba build/games/gba_bios.bin build/games/emerald_route_grass.svs on 32 1 1795091696 1000 -
    start rtc_gbz_state_rb2 "$QEMU" -cpu arm926 build/rtc_test build/games/emerald.gbz.tns build/games/gba_bios.bin build/games/emerald_route_grass.svs on 2 1 1795091696 1000 -
  fi
fi

aw="$GBA_GAMES/Advance Wars (USA) (Rev 1) (damaged).gba"
if [ -f "$aw" ] && [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  mkdir -p build/games build/replays
  ln -sf "$aw" build/games/aw_damaged.gba
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  : > build/replays/no_keys
  start aw_damaged "$QEMU" -cpu arm926 build/replay build/games/aw_damaged.gba build/games/gba_bios.bin - 32 0 300 \
    build/replays/no_keys
fi

start_run blend build/blend_test
start_run fill build/fill_test
start_run dynarec0 build/dynarec_test build/ram_code.gba 0
start_run dynarec1 build/dynarec_test build/ram_code.gba 1
[ $pcword_built = 0 ] && start_run pcword build/pcword/dynarec_test build/pcword/ram_code.gba 0
start_run badjump_thumb build/badjump_test build/bad_jump_thumb.gba 910803DE
start_run badjump_arm build/badjump_test build/bad_jump_arm.gba 0E000000
start_run dispatch build/dispatch_test build/ram_code.gba
start_run block_check build/block_check_test
start_run idle build/idle_test build/idle_loop.gba
# Advance Wars' idle pass (speed9b, main.c): the check build runs every pass
# and checks each event update_gba would have run without it.
awgame="$GBA_GAMES/Advance Wars (USA) (Rev 1).gba"
idlepass=0
if [ $idlecheck_built = 0 ] && [ -f "$awgame" ] && [ -f "$GBA_GAMES/gba_bios.bin" ] &&
   [ -f "$GBA_GAMES/aw_latest.svs.tns" ]; then
  idlepass=1
  mkdir -p build/games
  ln -sf "$awgame" build/games/aw.gba
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  ln -sf "$GBA_GAMES/aw_latest.svs.tns" build/games/aw_latest.svs
  start idlepass_state "$QEMU" -cpu arm926 build/idlecheck/perf build/games/aw.gba build/games/gba_bios.bin \
    build/games/aw_latest.svs 600 - 1
  start idlepass_boot "$QEMU" -cpu arm926 build/idlecheck/perf build/games/aw.gba build/games/gba_bios.bin \
    - 2400 replay/boot.keys 1
fi
# speed10b's kept RAM blocks (cpu_threaded.c), from power-on: Golden Sun
# rewrites its IWRAM code (1,197 flushes in 2,400 frames, 23,110 blocks
# copied), Advance Wars and Emerald now and then.
gsgame="$GBA_GAMES/Golden Sun (USA, Europe).gba"
keepgames=()
if [ $keep_built = 0 ]; then
  start_run keep_dynarec build/keepcheck/dynarec_test build/keepcheck/ram_code.gba 0
  start_run ktr_dynarec build/ktr/dynarec_test build/ktr/ram_code.gba 0
  start_run ktrn_dynarec build/ktrn/dynarec_test build/ktrn/ram_code.gba 0
fi
if [ $keep_built = 0 ] && [ -f "$gsgame" ] && [ -f "$awgame" ] && [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  mkdir -p build/games
  ln -sf "$gsgame" build/games/gs.gba
  ln -sf "$awgame" build/games/aw.gba
  ln -sf "$GBA_GAMES/gba_bios.bin" build/games/gba_bios.bin
  keepgames=(gs aw)
  if [ -f "$emerald" ]; then
    ln -sf "$emerald" build/games/em.gba
    keepgames+=(em)
  fi
  for g in "${keepgames[@]}"; do
    # the small PC table: the most entries each game's blocks run with
    # whose flushes come often (Golden Sun 1,230 in all, Advance Wars 214,
    # Emerald 259; 1,100 stops Golden Sun: a block of more)
    pcs=400
    [ $g = gs ] && pcs=1200
    start keepcheck_$g "$QEMU" -cpu arm926 build/keepcheck/perf build/games/$g.gba build/games/gba_bios.bin \
      - 2400 replay/boot.keys 1
    start keepcheckx_$g "$QEMU" -cpu arm926 build/keepcheckx/perf build/games/$g.gba build/games/gba_bios.bin \
      - 2400 replay/boot.keys 1 rampcs=$pcs
    for b in ktr ktrn ktr16 ktr16n; do
      start ${b}_$g "$QEMU" -cpu arm926 build/$b/perf build/games/$g.gba build/games/gba_bios.bin \
        - 2400 replay/boot.keys 1 every=1 state=build/jobs/${b}_$g.state
    done
    # (the store's positions from 4 KB under 2^31: started over on the way)
    for b in ktr32 ktr32n; do
      start ${b}_$g "$QEMU" -cpu arm926 build/$b/perf build/games/$g.gba build/games/gba_bios.bin \
        - 2400 replay/boot.keys 1 every=1 state=build/jobs/${b}_$g.state keepw=2147479552
    done
    for b in ktrx ktrxn; do
      start ${b}_$g "$QEMU" -cpu arm926 build/$b/perf build/games/$g.gba build/games/gba_bios.bin \
        - 2400 replay/boot.keys 1 every=1 state=build/jobs/${b}_$g.state rampcs=$pcs
    done
  done
fi
# States and pictures packed for nspire_states.c, with the reference states
# (unpacked) among the inputs when $GBA_GAMES has them.
fd_states=()
mkdir -p build/games
for s in emerald_route_grass aw_latest; do
  if [ -f "$GBA_GAMES/$s.svs.tns" ]; then
    ln -sf "$GBA_GAMES/$s.svs.tns" build/games/$s.svs
    fd_states+=(build/games/$s.svs)
  fi
done
start_run fastdeflate build/fastdeflate_test ${fd_states[@]+"${fd_states[@]}"}
start_run printf build/printf_test build/printf_test.txt
start_run printf_ref build/printf_test_ref build/printf_test_ref.txt
# The calculator build's program file, packed (tools/zehn_pack.py), against
# genzehn's own file of the same ELF (both made by the top folder's make).
zehn=0
# tests/gate.sh builds the calculator program beside this run and says in $GPSP_ZEHN_READY when it is made
if [ -n "${GPSP_ZEHN_READY:-}" ]; then
  for _ in $(seq 1800); do [ -s "$GPSP_ZEHN_READY" ] && break; sleep 1; done
  [ "$(cat "$GPSP_ZEHN_READY")" = ok ] || rm -f ../../gpsp_libretro.zehn
fi
if [ -f ../../gpsp_libretro.zehn ] && [ -f ../../gpsp_libretro.tns ]; then
  zehn=1
  start_run zehn build/zehn_test ../../gpsp_libretro.zehn ../../gpsp_libretro.tns
fi

# ---- The results ----

run blend blend_test
run fill fill_test
run dynarec0 "dynarec_test, Partial SMC"
run dynarec1 "dynarec_test, Classic SMC"
# The PC word check build (core-17's oracle, PCWORD_CHECK=1) on the same ROM:
# its ignored stores returned past a u32_safe call (step 13 crashed it).
if [ $pcword_built = 0 ]; then
  run pcword "dynarec_test, PC word check build"
else
  echo "FAIL: the PC word check build failed"; failed=$((failed + 1))
fi
run badjump_thumb "badjump_test, Thumb"
run badjump_arm "badjump_test, ARM"
run dispatch dispatch_test
run block_check block_check_test
run idle "idle_test, pure idle loop (core-16)"
idlepass_result() {   # ID NAME: repeats checked, none wrong, every pass of the one cost
  local line
  finish "$1"
  line=$(grep '^idle pass check:' "build/jobs/$1.out")
  if [ $status = 0 ] && echo "$line" | grep -q ' repeats as the pass ran, 0 not; 0 passes of another cost' &&
     ! echo "$line" | grep -q '^idle pass check: 0 repeats'; then
    echo "PASS: $2: ${line#idle pass check: }"
  else
    echo "FAIL  $2: $([ $status -gt 128 ] && echo "crashed (signal $((status - 128)))" || echo "exit $status") ${line:-no check line}"
    failed=$((failed + 1))
  fi
}
if [ $idlepass = 1 ]; then
  idlepass_result idlepass_state "Advance Wars' idle pass (speed9b), the reference name entry, 600 frames"
  idlepass_result idlepass_boot "Advance Wars' idle pass (speed9b), from power-on, 2400 frames"
elif [ $idlecheck_built != 0 ]; then
  echo "FAIL: the idle check build failed"; failed=$((failed + 1))
else
  echo "skipped: Advance Wars' idle pass (no game, BIOS or state in $GBA_GAMES)"
fi

keep_result() {   # GAME NAME
  local line b a n what more
  for b in keepcheck keepcheckx; do
    what=$2
    [ $b = keepcheckx ] && what="$2, a small PC table and ROM cache"
    finish ${b}_$1
    line=$(grep '^kept-block check:' "build/jobs/${b}_$1.out" | tail -1)
    more=$(grep '^kept blocks:' "build/jobs/${b}_$1.out" | tail -1)
    # every copy as its translation, every predicted flush made; copies
    # checked, and with the small PC table and ROM cache predicted flushes
    # and ROM flushes that emptied the store
    if [ $status = 0 ] && echo "$line" | grep -q ' blocks as made, 0 not; [0-9]* predicted flushes as made, 0 not;' &&
       ! echo "$line" | grep -q 'check: 0 blocks' &&
       { [ $b = keepcheck ] || { ! echo "$line" | grep -q '; 0 predicted' &&
                                 ! echo "$more" | grep -q 'ROM flush 0 times'; }; }; then
      echo "PASS: kept RAM blocks (speed10b), $what: ${line#kept-block check: }; ${more#kept blocks: }"
    else
      echo "FAIL  kept RAM blocks (speed10b), $what: exit $status ${line:-no check line}"
      failed=$((failed + 1))
    fi
  done
  for b in ktr ktr16 ktr32 ktrx; do
    case $b in
      ktr) what="the full-size RAM cache" ;;
      ktr32) what="a 32 KB RAM cache, its store's positions crossing 2^31" ;;
      ktrx) what="a small PC table and ROM cache" ;;
      *) what="a ${b#ktr} KB RAM cache" ;;
    esac
    finish ${b}_$1
    a=$status
    finish ${b}n_$1
    n=$status
    # every frame, every block made, the RAM cache's code, tag table, PC
    # table and tags as each frame ends, the whole state and the flushes as
    # without the keeping, blocks copied (and the store started over at
    # 2^31, emptied by ROM flushes)
    if [ $a = 0 ] && [ $n = 0 ] && grep -q '^frame 2400 ' build/jobs/${b}_$1.out &&
       cmp -s <(grep '^frame\|^keep-trace' build/jobs/${b}_$1.out) <(grep '^frame\|^keep-trace' build/jobs/${b}n_$1.out) &&
       cmp -s build/jobs/${b}_$1.state build/jobs/${b}n_$1.state &&
       [ "$(grep -o 'cache flushes ROM [0-9]* RAM [0-9]*' build/jobs/${b}_$1.out)" = \
         "$(grep -o 'cache flushes ROM [0-9]* RAM [0-9]*' build/jobs/${b}n_$1.out)" ] &&
       grep -q 'RAM blocks copied [1-9]' build/jobs/${b}_$1.out &&
       { [ $b != ktr32 ] || ! grep -q 'started over at 2^31 0$' build/jobs/${b}_$1.out; } &&
       { [ $b != ktrx ] || ! grep -q 'emptied by a ROM flush 0 times' build/jobs/${b}_$1.out; }; then
      echo "PASS: kept RAM blocks (speed10b), $2, $what: frames, blocks made, the RAM cache and the state as without them" \
        "($(grep -o 'cache flushes.*' build/jobs/${b}_$1.out | sed 's/, ROM cache.*;/;/'); $(grep '^kept blocks:' build/jobs/${b}_$1.out | sed 's/^kept blocks: //'))"
    else
      echo "FAIL  kept RAM blocks (speed10b), $2, $what: not as without them (exit $a/$n)"
      failed=$((failed + 1))
    fi
  done
}
if [ $keep_built = 0 ]; then
  # dynarec_test's ROM (its BIOS calls from IWRAM, made again after a flush:
  # a copy's branch into the ROM cache moved): checked, and traced as
  # without the keeping
  finish keep_dynarec
  line=$(grep '^kept-block check:' build/jobs/keep_dynarec.out)
  if [ $status = 0 ] && grep -q '^PASS' build/jobs/keep_dynarec.out &&
     echo "$line" | grep -q ' blocks as made, 0 not; [1-9][0-9]* copies linking into the ROM cache' &&
     ! echo "$line" | grep -q 'check: 0 blocks'; then
    echo "PASS: kept RAM blocks (speed10b), rom/ram_code.S: ${line#kept-block check: }"
  else
    echo "FAIL  kept RAM blocks (speed10b), rom/ram_code.S: exit $status ${line:-no check line}"
    failed=$((failed + 1))
  fi
  finish ktr_dynarec
  a=$status
  finish ktrn_dynarec
  if [ $a = 0 ] && [ $status = 0 ] && grep -q '^PASS' build/jobs/ktr_dynarec.out &&
     cmp -s <(grep -v '^RAM blocks copied' build/jobs/ktr_dynarec.out) <(grep -v '^RAM blocks copied' build/jobs/ktrn_dynarec.out) &&
     grep -q '^RAM blocks copied [1-9]' build/jobs/ktr_dynarec.out; then
    echo "PASS: kept RAM blocks (speed10b), rom/ram_code.S traced: as without them ($(grep '^RAM blocks copied' build/jobs/ktr_dynarec.out))"
  else
    echo "FAIL  kept RAM blocks (speed10b), rom/ram_code.S traced: not as without them (exit $a/$status)"
    failed=$((failed + 1))
  fi
fi
if [ ${#keepgames[@]} -gt 0 ]; then
  keep_result gs "Golden Sun from power-on, 2400 frames"
  keep_result aw "Advance Wars from power-on, 2400 frames"
  [ ${#keepgames[@]} -gt 2 ] && keep_result em "Emerald from power-on, 2400 frames"
elif [ $keep_built != 0 ]; then
  echo "FAIL: the kept blocks' check builds failed"; failed=$((failed + 1))
else
  echo "skipped: kept RAM blocks (no Golden Sun, Advance Wars or BIOS in $GBA_GAMES)"
fi

run fastdeflate fastdeflate_test
# newlib's printf family as the calculator links it (nspire_cold.mk
# PRINTF_LDFLAGS) against newlib's own: the same output for every call.
finish printf_ref
ref_out=$(cat build/jobs/printf_ref.out)
run printf "printf_test, the calculator's printf"
if [ $status = 0 ]; then
  if [ -n "$ref_out" ] && [ "$(cat build/jobs/printf.out)" = "$ref_out" ]; then
    echo "PASS: printf_test: the calculator's printf family gives newlib's own output"
  else
    echo "FAIL  printf_test: the calculator's printf family differs from newlib's own ($ref_out)"
    failed=$((failed + 1))
  fi
fi
if [ $zehn = 1 ]; then
  run zehn "zehn_test, the packed program file"
else
  echo "skipped: the packed program file (no gpsp_libretro.zehn and .tns: make in the top folder)"
fi

# Pokemon Emerald from a save state: into the tall grass until a wild
# Pokemon appears (its battle wipe draws window edges on odd pixels; the
# battle text is up by frame 1300). video.cc crashed in the wipe and Partial
# SMC froze at once. Every setting has to get there and draw the frames in
# replay/emerald_grass.frames (a hash of every 50th frame; the last one is the
# battle's). Those hashes are what video.cc and the legacy renderer (removed
# 2026-11-11) both drew, frame for frame: a change to video.cc's drawing
# that shows here is a change from what both drew, so look at the pictures
# (build/replay ... DIR) before storing new ones.
check_replay() {   # rom policy rombuf
  local rom=$1 policy=$2 rombuf=$3 name out
  name="$rom, $([ $policy = 0 ] && echo Partial || echo Classic) SMC, ${rombuf} MB ROM buffer"
  out=build/replays/$rom-p$policy-b$rombuf.txt
  finish "replay-$rom-p$policy-b$rombuf"
  mv "build/jobs/replay-$rom-p$policy-b$rombuf.out" "$out"
  replays=$((replays + 1))
  if [ $status -ne 0 ]; then
    echo "FAIL  Emerald replay, $name: $([ $status -gt 128 ] && echo "crashed (signal $((status - 128)))" || echo "exit $status") at $(grep frame "$out" | tail -1 | cut -d' ' -f2 || echo 0)"
    replay_failed=$((replay_failed + 1))
  elif [ "$(tail -1 "$out")" != "$EMERALD_LAST" ]; then
    echo "FAIL  Emerald replay, $name: last frame $(tail -1 "$out"), expected $EMERALD_LAST (the battle)"
    replay_failed=$((replay_failed + 1))
  elif ! cmp -s $EMERALD_FRAMES "$out"; then
    echo "FAIL  Emerald replay, $name: frames differ from $EMERALD_FRAMES, from $(diff $EMERALD_FRAMES "$out" | sed -n 's/^> frame \([0-9]*\).*/\1/p' | head -1)"
    replay_failed=$((replay_failed + 1))
  fi
}
if [ $emerald_replays = 1 ]; then
  replays=0 replay_failed=0
  each_replay check_replay
  echo "$([ $replay_failed = 0 ] && echo PASS || echo FAIL): Emerald replay: $((replays - replay_failed)) of $replays settings reached the battle with the stored frames"
  [ $replay_failed = 0 ] || failed=$((failed + 1))
else
  echo "skipped: Emerald replay (no game, BIOS or state in $GBA_GAMES)"
fi

check_rtc() {   # id expected_tail [extra_grep]
  local out="build/jobs/$1.out"
  finish "$1"
  rtc_runs=$((rtc_runs + 1))
  if [ $status -ne 0 ]; then
    echo "FAIL  Emerald RTC ($1): exit $status ($(tail -1 "$out"))"
    rtc_failed=$((rtc_failed + 1))
  elif [ "$(tail -1 "$out")" != "$2" ]; then
    echo "FAIL  Emerald RTC ($1): got '$(tail -1 "$out")', expected '$2'"
    rtc_failed=$((rtc_failed + 1))
  elif [ -n "${3:-}" ] && ! grep -q "$3" "$out"; then
    echo "FAIL  Emerald RTC ($1): missing '$3'"
    rtc_failed=$((rtc_failed + 1))
  fi
}
if [ $emerald_rtc = 1 ]; then
  rtc_runs=0 rtc_failed=0
  cx2_ok="rtc status 40 error 0000 probe 01 time 26 11 19 04 12 34 56 40"
  cx_ok="rtc status 40 error 0000 probe 01 time 70 01 01 04 01 42 32 40"
  off_ok="rtc status 40 error 0090 probe 11 time 00 00 00 00 00 00 00 00"
  check_rtc rtc_auto_cx2 "$cx2_ok"
  check_rtc rtc_on_cx2 "$cx2_ok"
  check_rtc rtc_on_cx "$cx_ok"
  check_rtc rtc_off "$off_ok"
  check_rtc rtc_gbz_rb2 "$cx2_ok"
  check_rtc rtc_newgame "$cx2_ok" "^frame 7000: start over$"
  if [ $emerald_replays = 1 ]; then
    check_rtc rtc_state_auto "$cx2_ok" "^state rtc status 40 error 0000$"
    check_rtc rtc_state_on "$cx2_ok" "^state rtc status 40 error 0000$"
    check_rtc rtc_gbz_state_rb2 "$cx2_ok" "^state rtc status 40 error 0000$"
  fi
  echo "$([ $rtc_failed = 0 ] && echo PASS || echo FAIL): Emerald RTC: $((rtc_runs - rtc_failed)) of $rtc_runs checks passed (power-on, CX 1970, .gbz 2 MB, New Game -> Start over, state reset)"
  [ $rtc_failed = 0 ] || failed=$((failed + 1))
else
  echo "skipped: Emerald RTC (no game or BIOS in $GBA_GAMES)"
fi

# The reference Advance Wars, a bad dump: a function pointer of its reads as
# 0x910803DF, and the game jumps there as it starts (the calculator reset).
# gpSP has to stop there instead.
if [ -f "$aw" ] && [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  finish aw_damaged
  mv build/jobs/aw_damaged.out build/replays/aw_damaged.txt
  if [ $status = 3 ] && grep -q "stopped at 910803DE" build/replays/aw_damaged.txt; then
    echo "PASS: the damaged Advance Wars stops at its jump to 910803DE ($(cut -d' ' -f1-2 build/replays/aw_damaged.txt))"
  else
    echo "FAIL  the damaged Advance Wars: $([ $status -gt 128 ] && echo "crashed (signal $((status - 128)))" || echo "exit $status: $(tail -1 build/replays/aw_damaged.txt)")"
    failed=$((failed + 1))
  fi
else
  echo "skipped: the damaged Advance Wars (not in $GBA_GAMES)"
fi

# Drawing in batches (video.h: lines drawn when the game next changes what
# the picture is made of, or at VBlank) has to give the same frames as
# drawing each line at its HBlank: every frame's hash, from
# the reference states and from power-on through intros and title screens
# (the games' own windows, blending, affine and mid-frame changes).
# The line-by-line run is also compared with replay/batch_c<N>.frames (a hash
# of every 50th frame, stored when the legacy renderer was removed: they were
# equal to the legacy-era recordings), so a video.cc change that moves both
# modes alike fails here; update them only for an intended picture change.
if [ -f "$GBA_GAMES/gba_bios.bin" ]; then
  for i in "${!batch_cases[@]}"; do
    if ! batch_ready "$i"; then
      IFS='|' read -r name _ <<< "${batch_cases[$i]}"
      echo "skipped: batched drawing, $name (not in $GBA_GAMES)"
      continue
    fi
    for b in 0 1 w; do finish "batch$i-$b"; done
    [ $linecheck_built = 0 ] && finish "batch$i-c"
  done
  batch_failed=0 batch_runs=0 line_kept=
  for i in "${!batch_cases[@]}"; do
    IFS='|' read -r name _ _ _ frames _ <<< "${batch_cases[$i]}"
    f=build/batch/c$i
    [ -f $f-b1.txt ] || continue
    batch_runs=$((batch_runs + 1))
    if [ "$(grep -c '^frame' $f-b0.txt)" -lt "$frames" ] || grep -q '^exit\|stopped' $f-b0.txt; then
      echo "FAIL  batched drawing, $name: the run drawing each line didn't finish: $(tail -1 $f-b0.txt)"
      batch_failed=$((batch_failed + 1))
    elif ! awk '$1 == "frame" && $2 % 50 == 0' $f-b0.txt | cmp -s - replay/batch_c$i.frames; then
      echo "FAIL  batched drawing, $name: frames differ from the stored replay/batch_c$i.frames (every 50th frame)"
      batch_failed=$((batch_failed + 1))
    elif ! cmp -s $f-b0.txt $f-b1.txt; then
      echo "FAIL  batched drawing, $name: frames differ from $(diff $f-b0.txt $f-b1.txt | sed -n 's/^> frame \([0-9]*\).*/\1/p' | head -1)"
      batch_failed=$((batch_failed + 1))
    elif ! cmp -s $f-b1.txt $f-b1-w.txt; then
      # (speed9a: the OS's tick watch armed on every line, its call trashing
      # r0-r3, r12 and the flags, from update_gba and the store handlers)
      echo "FAIL  batched drawing, $name: with the tick watch's call trashing registers, frames differ from $(diff $f-b1.txt $f-b1-w.txt | sed -n 's/^> frame \([0-9]*\).*/\1/p' | head -1)"
      batch_failed=$((batch_failed + 1))
    elif [ $linecheck_built = 0 ]; then
      # (speed10: the lines kept as they were, drawn anyway in the check
      # build and compared: none differs, and its frames, every line drawn,
      # are the ones kept lines gave)
      line=$(grep '^line check:' $f-c.txt)
      if ! echo "$line" | grep -q ', 0 not$'; then
        echo "FAIL  batched drawing, $name: line check build: ${line:-no check line ($(tail -1 $f-c.txt))}"
        batch_failed=$((batch_failed + 1))
      elif ! grep '^frame' $f-c.txt | cmp -s - <(grep '^frame' $f-b1.txt); then
        echo "FAIL  batched drawing, $name: every line drawn (the line check build), frames differ from $(diff <(grep '^frame' $f-b1.txt) <(grep '^frame' $f-c.txt) | sed -n 's/^> frame \([0-9]*\).*/\1/p' | head -1)"
        batch_failed=$((batch_failed + 1))
      else
        line_kept="$line_kept${line_kept:+; }${name%%,*}: ${line#line check: }"
      fi
    fi
  done
  echo "$([ $batch_failed = 0 ] && echo PASS || echo FAIL): batched drawing: $((batch_runs - batch_failed)) of $batch_runs runs drew the same frames as line by line (and with the tick watch's call trashing r0-r3, r12, the flags; and as every line drawn, the kept ones compared: ${line_kept:-none})"
  [ $batch_failed = 0 ] || failed=$((failed + 1))
  if [ $linecheck_built != 0 ]; then
    echo "FAIL: the line check build failed"; failed=$((failed + 1))
  fi
else
  echo "skipped: batched drawing (no BIOS in $GBA_GAMES)"
fi

# (what took the time: the longest runs, in seconds each took with the others beside it)
echo "slowest runs (s): $(for f in build/jobs/*.secs; do echo "$(cat "$f") $(basename "$f" .secs)"; done | sort -rn | head -8 | tr '\n' ',' | sed 's/,$//')"
[ $failed = 0 ] && echo "ARM tests: all passed" || echo "ARM tests: $failed failed"
[ $failed = 0 ]
