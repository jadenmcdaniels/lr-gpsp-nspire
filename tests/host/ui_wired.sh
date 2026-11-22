#!/bin/sh
# One PNG per real screen, drawn through the real menu code (gui.c and the
# other screens, on gpsp-host, the calculator's whole frontend on the PC):
#   tests/host/ui_wired.sh OUTDIR [SCENE...]
# Scenes: list list_names_and_battery menu settings link game controls
# questions no_game speed_test loading loading_zip managers court (the main
# screens with real games, court-*.png: needs $GBA_GAMES); r16: drive_saves
# bluetooth toasts (the screens other work brings, with made-up words).
# GPSP_TEST_JOBS=N (default half the CPUs): N scenes at a time (par.py), the
# same PNGs and output; 1: one after another.
set -e
cd "$(dirname "$0")"
JOBS=${GPSP_TEST_JOBS:-$(($(nproc) / 2))}
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
make -s -j"$JOBS"
[ "$JOBS" -le 1 ] && exec python3 -I ui_wired.py "$@"
exec python3 -I par.py --jobs "$JOBS" ui_wired.py "$@"
