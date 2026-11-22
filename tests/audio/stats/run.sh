#!/bin/bash
# How often the mixer runs and what it writes, per scene (the baseline
# numbers): builds a copy of the tree with the counters
# of counters.patch (in sound.c and the harness: a measuring build, never
# committed) in a scratch folder, runs the five scenes under plain qemu-arm and
# prints the counters per frame. Needs the patch to apply to sound.c as it is:
# after the mixer is rewritten, update the patch first.
#   tests/audio/stats/run.sh [SCRATCHDIR]
. "$(dirname "$0")/../lib.sh"
D=${1:-$(mktemp -d "${TMPDIR:-/tmp}/audiostats.XXXXXX")}
mkdir -p "$D/tree"
git -C "$ROOT" archive HEAD | tar -x -C "$D/tree"
cp "$ROOT/tests/audio/perf_audio.h" "$D/tree/tests/audio/perf_audio.h"
cp "$ROOT/libretro-gpsp/sound.c" "$D/tree/libretro-gpsp/sound.c"
(cd "$D/tree" && patch -s -p1 <"$ROOT/tests/audio/stats/counters.patch") || exit 1
make -s -C "$D/tree/tests/arm" -j4 AUDIO=1 build/perf_audio 2>&1 | grep -v '^lto'
(
  . "$D/tree/tests/audio/lib.sh"
  for s in $SCENE_NAMES; do
    scene_get "$s" || continue
    perf_functional perf_audio "$s" "$SC_FRAMES" every=1000 statfrom="$SC_SKIP" | grep '^SST' >"$D/sst_$s.txt"
  done
)
python3 -I "$ROOT/tests/audio/stats/sstparse.py" "$D"
