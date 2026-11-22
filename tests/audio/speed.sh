#!/bin/bash
# What mixing costs on the calculator, from the speed model (tests/calc_speed.py,
# the ARM926 model), for every test scene (the games, states and keys of
# tests/speed_scenes.sh, their full frame counts), without and with the mixer:
#
#   tests/audio/speed.sh [--calc cx2|cx192|cx] [--scenes "aw emerald"] [--top N] [--set K=V]...
#
# One line per scene: the game's ms per frame without audio (NSPIRE_NO_AUDIO,
# build/perf) and with audio (build/perf_audio), the difference, the audio
# part alone (sound_timer + render_gbc_sound + sound_read_samples) in ms and
# in model cycles per frame, and its share of the frame (the whole frame:
# game, drawing and picture). --top N: the audio part's N costliest functions
# per scene. --set: mixer settings for the audio harness (key=value).
# Runs JOBS model runs at a time (default $GPSP_TEST_JOBS, or half the CPUs;
# 10 runs, about 10 minutes one at a time); the numbers are the same in any
# tree and shell (lib.sh: model_env). Times are the model's times times 1.05,
# as calc_speed.py prints them.
. "$(dirname "$0")/lib.sh"
calc=cx2; scenes=$SCENE_NAMES; top=0; sets=()
while [ $# -gt 0 ]; do
  case $1 in
    --calc) calc=$2; shift ;;
    --scenes) scenes=$2; shift ;;
    --top) top=$2; shift ;;
    --set) sets+=("$2"); shift ;;
    *) echo "usage: speed.sh [--calc cx2|cx192|cx] [--scenes \"aw ...\"] [--top N] [--set K=V]"; exit 2 ;;
  esac
  shift
done
build_harness || exit 1
S=$(mktemp -d "${TMPDIR:-/tmp}/audiospeed.XXXXXX")
trap 'rm -rf "$S"' EXIT
setargs=()
for kv in "${sets[@]}"; do setargs+=(--set "$kv"); done
njobs=${JOBS:-${GPSP_TEST_JOBS:-$(($(nproc) / 2))}}
start() {   # NAME COMMAND...: in the background, JOBS at a time; NAME.ok when it worked
  local name=$1
  shift
  while [ "$(jobs -rp | wc -l)" -ge "$njobs" ]; do wait -n; done
  { "$@" && : > "$S/$name.ok"; } &
}
for s in $scenes; do
  scene_get "$s" || { echo "$s: game or state missing, left out" >&2; continue; }
  start "$s.noaudio" eval 'model_run spd$s "$s" "$SC_FRAMES" --calc "$calc" >"$S/$s.noaudio.txt"'
  start "$s.audio" eval 'PERF_AUDIO=1 model_run spa$s "$s" "$SC_FRAMES" --calc "$calc" --top 400 "${setargs[@]}" >"$S/$s.audio.txt"'
done
wait
for s in $scenes; do
  scene_get "$s" || continue
  [ -f "$S/$s.noaudio.ok" ] && [ -f "$S/$s.audio.ok" ] || exit 1
  cp "$ARM/build/speed/spd$s.model" "$S/$s.noaudio.model"
  cp "$ARM/build/speed/spa$s.model" "$S/$s.audio.model"
  echo "$s done" >&2
done
python3 -I "$AUDIO_DIR/speedtab.py" "$calc" "$top" "$S" $scenes
