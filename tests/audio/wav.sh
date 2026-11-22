#!/bin/bash
# A listening clip: the audio the mixer makes in a scene, as a WAV file
# (16 bit stereo, the mixer's rate, 65536 Hz by default), and its level.
#
#   tests/audio/wav.sh SCENE OUT.wav [SETTING=VALUE...]
#
# The clip is about 10 seconds of the scene (CLIP frames at the end of its
# run; see lib.sh), from the audio harness under qemu-arm. Settings are the
# harness's mixer settings (key=value, tests/audio/settings.txt). Prints the
# file's peak and RMS per side, and the share of silent samples.
. "$(dirname "$0")/lib.sh"
[ $# -ge 2 ] || { echo "usage: wav.sh SCENE OUT.wav [SETTING=VALUE...]"; exit 2; }
scene=$1; out=$2; shift 2
scene_get "$scene" || { echo "scene $scene: unknown, or its game or state is missing" >&2; exit 1; }
build_harness || exit 1
clip=$(echo "$CLIPS" | grep "^$scene|") || exit 1
IFS='|' read -r _ from to <<<"$clip"
mkdir -p "$(dirname "$out")"
out=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
perf_functional perf_audio "$scene" "$to" every=100 "wav=$out" "wavfrom=$from" "wavto=$to" "$@" | tail -2
python3 -I "$AUDIO_DIR/wavstat.py" "$out"
