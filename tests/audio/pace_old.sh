#!/bin/bash
# The pacing model (pace_test.c) on an older tree's nspire_audio.c and
# nspire_frameskip.c, with its queue of transfers: by default faf629e's
# (before 2026-11-15's OS time, the sound's rate and the frameskip while
# sound plays; 6 transfers), reporting only (--old: no checks).
#   tests/audio/pace_old.sh [COMMIT [TRANSFERS]] [-- pace_test arguments]
# Prints pace_test's lines; the build is tests/host/build-sink-old.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
commit=faf629e; nx=6
if [ $# -gt 0 ] && [ "$1" != -- ]; then commit=$1; shift; fi
if [ $# -gt 0 ] && [ "$1" != -- ]; then nx=$1; shift; fi
[ "${1:-}" = -- ] && shift
B=build-sink-old
SRC=$ROOT/tests/host/$B/src
mkdir -p "$SRC"
git -C "$ROOT" archive "$commit" nspire_audio.c nspire_frameskip.c | tar -x -C "$SRC"
# (a fresh build: the old sources' objects and the driver's with its queue)
rm -f "$ROOT"/tests/host/$B/pace_*.o "$ROOT"/tests/host/$B/sink_*.o
make -s -C "$ROOT/tests/host" -j8 -f ../audio/pace.mk BUILD=$B SAN=1 GPSP_UA_NX="$nx" PACE_SRC="$SRC" \
  $B/pace_test >/dev/null 2>&1
exec "$ROOT/tests/host/$B/pace_test" --old "$@"
