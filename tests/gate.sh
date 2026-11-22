#!/bin/bash
# The release gate: every test of the release candidate, from a clean state of
# this tree, as parallel as is safe, and one summary at the end.
#
#   tests/gate.sh                    the whole gate
#   tests/gate.sh --part arm         one part (host ui drive audio arm calc; repeat, or a,b)
#   tests/gate.sh --list             the parts
#   tests/gate.sh --logs DIR         the logs there (default: a new /tmp/gpsp-gate.XXXXXX, printed)
#
# The parts (each one's whole output in LOGS/PART.log):
#   host   tests/host/run.sh: C unit suites, host_tests, pad, hub, diag, bt, seamless, audio sink and pace
#   ui     tests/host/ui_wired.sh: every screen as a picture, each one audited (the pictures: LOGS/ui-shots)
#   drive  tests/host/drive_tests.sh's two runs, drive_ui_tests.py and drive_save_tests.py on the FAT16 and
#          the FAT32 image (tests/host/par.py: every test of them side by side)
#   audio  tests/host/audio_ui_tests.py
#   arm    tests/arm/run.sh (qemu-arm), its builds and runs side by side
#   calc   the calculator build (tools/gpsp-build.sh), tests/calc_checks.py, tests/layout_pinned.py
# Clean state: the tests' build folders are emptied first (make clean), the calculator build is made from
# nothing. The parts run side by side (the PC build once, first, for the four that use it; the host part's
# tests that measure the clock go before the others start: see REALTIME); each one's work folders are its own. A part that fails, crashes, times out ($GATE_TIMEOUT seconds, default 2400), is
# skipped, runs fewer checks than tests/gate_floor.txt says, or prints no result is FAIL; the exit status is
# 0 only when every part asked for is PASS. GPSP_TEST_JOBS (default: all CPUs) is the jobs each part uses,
# GATE_JOBS_ARM and the like one part's (the parts share the CPUs, so a part's own time is its time under
# that load).
# Needs the Ndless toolchain (on PATH, or NSPIRE_ENV: a script that puts it there), qemu-arm ($QEMU), the
# GBA test ROMs ($GBA_TESTS) and the games in $GBA_GAMES, as the parts do.

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
PARTS="host ui drive audio arm calc"
NSPIRE_ENV=${NSPIRE_ENV:-}
TIMEOUT=${GATE_TIMEOUT:-2400}
NCPU=$(nproc)
# jobs a part uses (GPSP_TEST_JOBS: the same for all; GATE_JOBS_<PART>: one's own)
jobs_of() {
    local v=GATE_JOBS_$(echo "$1" | tr a-z A-Z)
    if [ -n "${!v:-}" ]; then echo "${!v}"; elif [ -n "${GPSP_TEST_JOBS:-}" ]; then echo "$GPSP_TEST_JOBS"
    else
        # the PC parts' tests are timed against the clock (a hang guard, the links' real time): few at a time;
        # arm and calc are throughput (they run niced, taking what the others leave)
        case $1 in host|ui) echo $((NCPU / 3)) ;; drive) echo $((NCPU / 4)) ;; audio) echo 1 ;; *) echo "$NCPU" ;; esac
    fi
}
W=$ROOT/tests/host/build/w        # (short: the link tests' sockets are in it)

now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.0f", b - a }'; }

# Priorities: host has the tests that measure real time (the links' frames a second, the pads', hub's and
# bluetooth's waits), so it runs as if alone: the other parts are niced to the lowest, taking only the CPUs
# it leaves (they are throughput: arm and the calculator build, and the ui and drive tests measure the run's
# own simulated time).
niceness() { case $1 in host|audio) echo 0 ;; *) echo 19 ;; esac; }

# ---- one step of a part: its name, its time, and its status in the log ----
STEP_FAILED=0
step() {
    local name=$1 t0 t1 rc
    shift
    echo "=== step $name: $*"
    t0=$(now)
    "$@"
    rc=$?
    t1=$(now)
    echo "=== step $name: exit $rc, $(secs "$t0" "$t1") s"
    [ $rc -eq 0 ] || STEP_FAILED=1
    return $rc
}

toolchain() { [ -n "$NSPIRE_ENV" ] && [ -f "$NSPIRE_ENV" ] && . "$NSPIRE_ENV" >/dev/null 2>&1; }
# the calculator build from nothing (make clean, then make), as a user builds it
calc_make() { make -s clean > /dev/null 2>&1; make -j"$1"; }

# ---- the calculator build: also what arm's zehn test and calc_checks need ----
calc_build() {
    local rc
    toolchain
    step "calculator build" calc_make "$(jobs_of calc)" > "$LOGS/calc-build.out" 2>&1
    rc=$?
    cat "$LOGS/calc-build.out"
    if grep -q 'built without USB gamepads' "$LOGS/calc-build.out"; then
        echo "FAIL: built without USB gamepads"
        rc=1
    fi
    [ -f "$ROOT/gpsp_libretro.tns" ] && [ -f "$ROOT/gpsp_libretro.zehn" ] || { echo "FAIL: no gpsp_libretro.tns/.zehn after the build"; rc=1; }
    return $rc
}

part_calc() {
    local d
    calc_build
    if [ $? -eq 0 ]; then echo ok > "$LOGS/zehn.ready"; else echo failed > "$LOGS/zehn.ready"; fi
    toolchain
    step "calc_checks" python3 -I tests/calc_checks.py
    # layout_pinned adds code to gui.c while it runs and builds: in a copy of the tree (what is in it now,
    # the tracked files and the new ones), so that nothing else compiling gui.c sees that
    # (under TMPDIR when it is set: /tmp can be a small, quota'd tmpfs, and a full one fails the gate's other
    # parts too: their compilers' temporary files go there)
    d=$(mktemp -d "${TMPDIR:-/tmp}/gpsp-gate-layout.XXXXXX") || return 1
    mkdir "$d/gpsp" || return 1
    ( git ls-files -z --cached --others --exclude-standard | tar --null -T - -cf - ) | tar -x -C "$d/gpsp" || return 1
    local l
    for l in "$ROOT"/../*; do
        [ -L "$l" ] && ln -s "$(readlink -f "$l")" "$d/$(basename "$l")"
    done
    GPSP_LAYOUT_ROOT=$d/gpsp step "layout_pinned" python3 -I tests/layout_pinned.py
    local rc=$?
    # the copy goes when the check passed (it stayed in /tmp, 10 to 400 MB a run, until /tmp was full); a
    # failed check keeps it for a look, and says where
    if [ $rc -eq 0 ]; then
        rm -r "$d"
    else
        make -s -C "$d/gpsp" clean > /dev/null 2>&1
        echo "layout_pinned failed: its copy of the tree is kept at $d"
    fi
    return $rc
}

part_arm() {
    if [ "$CALC_SELECTED" = 0 ]; then
        calc_build || echo "FAIL: the calculator build failed (zehn_test has nothing to run on)"
        echo ok > "$LOGS/zehn.ready"
    fi
    toolchain
    GPSP_ZEHN_READY=$LOGS/zehn.ready step "tests/arm/run.sh" tests/arm/run.sh
}

part_host() { GPSP_TEST_WORK=$W/host step "tests/host/run.sh" tests/host/run.sh; }
part_ui() { GPSP_TEST_WORK=$W/ui step "tests/host/ui_wired.sh" tests/host/ui_wired.sh "$LOGS/ui-shots"; }
part_audio() { GPSP_TEST_WORK=$W/audio step "audio_ui_tests.py" python3 -I tests/host/audio_ui_tests.py; }
part_drive() {
    local plan=$LOGS/drive.plan img16=build/drive-work/fat16.img img32=build/drive-work/fat32.img
    : > "$plan"
    local cmd
    for cmd in "sh drive_tests.sh --run 16" "sh drive_tests.sh --run 32" \
               "python3 -I drive_ui_tests.py $img16" "python3 -I drive_ui_tests.py $img32" \
               "python3 -I drive_save_tests.py $img16" "python3 -I drive_save_tests.py $img32"; do
        printf '%s\t' $cmd >> "$plan"
        echo >> "$plan"
    done
    cd tests/host && GPSP_TEST_WORK=$W/drive step "par.py (drive plan)" python3 -I par.py --jobs "$GPSP_TEST_JOBS" --plan "$plan"
}

# ---- what a part leaves in its log: its counts, and whether they are a pass ----
# counts PART: "checks N failed M skipped K" (a part's own words for them)
count_lines() {   # PART LOG: prints "N M K"
    local part=$1 log=$2
    case $part in
    ui) awk '/^[A-Za-z0-9_.-]+\.png$/ { n++ } /^FAIL / { f++ } /(^|: )skipped|skipped \(no /{ s++ } END { printf "%d %d %d", n, f, s }' "$log" ;;
    arm) awk '/^PASS:/ { n++ } /^FAIL/ { f++ } /^skipped:/ { s++ } END { printf "%d %d %d", n, f, s }' "$log" ;;
    calc) awk '/^PASS:/ { n++ } /^FAIL/ { f++ } /^[0-9]+ failed$/ { n++; f += $1 } /skipped/ { s++ } END { printf "%d %d %d", n, f, s }' "$log" ;;
    *) awk '{ if (match($0, /[0-9]+ checks?, [0-9]+ failed/)) { split(substr($0, RSTART, RLENGTH), a, /[ ,]+/); n += a[1]; f += a[3] }
              if ($0 ~ /^FAIL/) f++
              if ($0 ~ /(^|: )skipped|skipped \(no /) s++ }
            END { printf "%d %d %d", n, f, s }' "$log" ;;
    esac
}
unit_of() { case $1 in ui) echo pictures ;; arm) echo results ;; calc) echo results ;; *) echo checks ;; esac; }

floor_of() { awk -v p="$1" '$1 == p { print $2 }' "$ROOT/tests/gate_floor.txt" 2>/dev/null; }

# ---- the internal entry: run one part in this process (gate.sh --internal PART LOGS) ----
if [ "${1:-}" = "--internal" ]; then
    LOGS=$3
    CALC_SELECTED=${GATE_CALC_SELECTED:-1}
    export GPSP_TEST_JOBS=$(jobs_of "$2") GPSP_TEST_HANG=${GPSP_TEST_HANG:-900}
    part_"$2"
    [ $STEP_FAILED -eq 0 ]
    exit $?
fi

# ---- the command line ----
SELECTED=
LOGS=${GATE_LOGS:-}
while [ $# -gt 0 ]; do
    case $1 in
    --part) shift; SELECTED="$SELECTED ${1//,/ }" ;;
    --part=*) SELECTED="$SELECTED ${1#--part=}" ;;
    --logs) shift; LOGS=$1 ;;
    --list) echo $PARTS; exit 0 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "gate.sh: unknown argument $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
SELECTED=${SELECTED//,/ }
[ -n "$SELECTED" ] || SELECTED=$PARTS
for p in $SELECTED; do
    case " $PARTS " in *" $p "*) ;; *) echo "gate.sh: no part $p (parts: $PARTS)" >&2; exit 2 ;; esac
done
has() { case " $SELECTED " in *" $1 "*) return 0 ;; esac; return 1; }

if [ -z "$LOGS" ]; then LOGS=$(mktemp -d "${TMPDIR:-/tmp}/gpsp-gate.XXXXXX") || exit 2; else mkdir -p "$LOGS"; fi
LOGS=$(cd "$LOGS" && pwd)
echo "gate: $(date '+%F %T'), $ROOT, parts:$(for p in $SELECTED; do printf ' %s' "$p"; done), load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "gate: logs in $LOGS"
mkdir -p "$LOGS/ui-shots"
: > "$LOGS/zehn.ready"   # (empty: not yet; arm waits for a word in it)
T0=$(now)

declare -A PID
# launch PART: the part in the background, its log, time and exit status in LOGS
launch() {
    local p=$1
    (
        s=$(now)
        GATE_CALC_SELECTED=$(has calc && echo 1 || echo 0) timeout -k 20 "$TIMEOUT" nice -n "$(niceness "$p")" "$ROOT/tests/gate.sh" --internal "$p" "$LOGS" > "$LOGS/$p.log" 2>&1
        rc=$?
        [ $rc -ne 124 ] || echo "TIMEOUT: $p did not finish in $TIMEOUT s" >> "$LOGS/$p.log"
        echo "$rc $(secs "$s" "$(now)")" > "$LOGS/$p.status"
    ) &
    PID[$p]=$!
}
# notrun PART WHY: a part that could not start is FAIL, said in its log
notrun() {
    echo "NOT RUN: $2" > "$LOGS/$1.log"
    echo "97 0" > "$LOGS/$1.status"
}

for p in $SELECTED; do : > "$LOGS/$p.status"; done   # (empty: not finished)

# The tests that measure the clock (the links' frames a second, in host_tests.py) run first and alone: with the
# other parts on the machine, even niced to the lowest, "link fast fall" came to 22.9 frames a second (it
# checks 25; alone it is 27-28). par.py runs them (GPSP_PAR_QUIET) before everything else of the host part and
# says when they are over; the other parts start then.
REALTIME=
for t in link_delay link_real_model link_fast link_fast_os link_hold_frames link_auto link_auto_relink link_exact; do
    REALTIME="$REALTIME${REALTIME:+,}host_tests.py:$t"
done
export GPSP_PAR_QUIET=$REALTIME GPSP_PAR_QUIET_DONE=$LOGS/quiet.done

# the PC build, once, for the parts that use it
PC_SELECTED=
for p in host ui drive audio; do has $p && PC_SELECTED="$PC_SELECTED $p"; done
if [ -n "$PC_SELECTED" ]; then
    (
        echo "=== PC build (make clean, make -j, the drive images)"
        s=$(now)
        rc=0
        make -s -C tests/host clean || rc=1
        [ $rc -eq 0 ] && { make -s -C tests/host -j"$(jobs_of host)" || rc=1; }
        if [ $rc -eq 0 ] && { has drive || has ui; }; then sh tests/host/drive_tests.sh --images || rc=1; fi
        echo "=== PC build: exit $rc, $(secs "$s" "$(now)") s"
        exit $rc
    ) > "$LOGS/pc-build.log" 2>&1
    PCRC=$?
    PCSECS=$(grep '^=== PC build: ' "$LOGS/pc-build.log" | sed 's/.*, \([0-9]*\) s/\1/')
    mkdir -p "$W"
fi
has arm && make -s -C tests/arm clean > "$LOGS/arm-clean.log" 2>&1
if has host; then
    if [ "${PCRC:-1}" -eq 0 ]; then launch host; else notrun host "the PC build failed (pc-build.log)"; fi
    # (the others wait for the quiet: its file, or the host part's end)
    while [ ! -e "$LOGS/quiet.done" ] && [ ! -s "$LOGS/host.status" ]; do sleep 1; done
fi
has calc && launch calc
has arm && launch arm
for p in ui drive audio; do
    has $p || continue
    if [ "${PCRC:-1}" -eq 0 ]; then launch $p; else notrun $p "the PC build failed (pc-build.log)"; fi
done

wait
T1=$(now)

# ---- the summary ----
echo
echo "gate summary ($LOGS)"
printf '%-6s %-5s %6s  %s\n' part state secs "counts"
ALL=0
[ -n "$PC_SELECTED" ] && printf '%-6s %-5s %6s  %s\n' build "$([ "${PCRC:-1}" -eq 0 ] && echo ok || echo FAIL)" "${PCSECS:-?}" "(the PC build, before the host, ui, drive and audio parts)"
for p in $SELECTED; do
    st=FAIL why=
    rc= secs=0
    read -r rc secs < "$LOGS/$p.status"
    if [ -z "$rc" ]; then
        rc=96 secs=0
        why="no status (the part's process was lost)"
    fi
    read -r n f s <<< "$(count_lines "$p" "$LOGS/$p.log")"
    fl=$(floor_of "$p")
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ] || grep -q '^TIMEOUT:' "$LOGS/$p.log"; then why="timed out"
    elif [ "$rc" -eq 97 ]; then why="not run: $(sed 's/^NOT RUN: //' "$LOGS/$p.log")"
    elif [ "$rc" -gt 128 ]; then why="crashed (signal $((rc - 128)))"
    elif [ "$rc" -ne 0 ]; then why="exit $rc"
    elif [ "${f:-0}" -ne 0 ]; then why="$f failed"
    elif [ "${s:-0}" -ne 0 ]; then why="$s skipped"
    elif [ "${n:-0}" -eq 0 ]; then why="no result lines"
    elif [ -n "$fl" ] && [ "$n" -lt "$fl" ]; then why="$n $(unit_of "$p"), fewer than the $fl of tests/gate_floor.txt"
    fi
    [ -n "$why" ] || st=PASS
    [ $st = PASS ] || ALL=1
    printf '%-6s %-5s %6s  %s\n' "$p" $st "$secs" "$n $(unit_of "$p"), $f failed, $s skipped${why:+; $why}"
done
echo "total $(secs "$T0" "$T1") s wall-clock; logs: $LOGS"
if [ $ALL -eq 0 ]; then echo "GATE: PASS"; else echo "GATE: FAIL"; fi
exit $ALL
