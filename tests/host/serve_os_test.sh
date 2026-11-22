#!/bin/sh
# states_serve_os (nspire_states.h) at the
# quit's state save: 0, as the quit always ran (the exit path's comments
# assume the OS doesn't run there), whether the quit comes from the game's
# Q key or from the paused menu's Quit (the menu's own saves and loads run
# served: 1). gpsp-host (build/gpsp-host, run.sh builds it) quits a game
# both ways with Save when I quit on; its report says the value
# (host_hw.c states_save_serve). Its folders: $GPSP_TEST_BUILD/serve_os,
# written over at each run.
set -e
cd "$(dirname "$0")"
BUILD=${GPSP_TEST_BUILD:-build}
HOST=./$BUILD/gpsp-host
GBA_TESTS=${GBA_TESTS:?set GBA_TESTS to a copy of github.com/jsmolka/gba-tests}
T=$BUILD/serve_os
fail=0
run() {
  name=$1 script=$2 want=$3
  mkdir -p "$T/$name/gba" "$T/$name/out"
  cp "$GBA_TESTS/arm/arm.gba" "$T/$name/gba/arm.gba.tns"
  python3 -I -c 'import sys; open(sys.argv[1], "wb").write(bytes(range(256)) * 16)' "$T/$name/gba/_gpSP.tns"
  printf '%b' "$script" > "$T/$name/script.txt"
  : > "$T/$name/out/report.txt"
  "$HOST" "$T/$name/script.txt" "$T/$name/out" "$T/$name/gba" > "$T/$name/out/stdout.txt" 2>&1 || true
  got=$(sed -n 's/^states_save_serve //p' "$T/$name/out/report.txt")
  if [ "$got" = "$want" ]; then
    echo "serve_os: $name: the quit's save with states_serve_os $got: ok"
  else
    echo "FAIL serve_os: $name: the quit's save with states_serve_os '$got', want $want"
    fail=1
  fi
}
run quit_key "wait 1\npress enter\nwait 1.5\npress Q\nwait 1\nquit\n" 0
run menu_quit "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\npress up\npress right\npress right\nwait 0.2\npress enter\nwait 0.5\npress enter\nwait 1\nquit\n" 0
exit $fail
