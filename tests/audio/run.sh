#!/bin/bash
# The audio gate: builds what it needs and exits 0 only if all hold, one
# PASS/FAIL line per check:
#  (1) default builds untouched: the no-audio speed harness gives exactly the
#      model numbers recorded at the trunk fbfb478 (ref/model_aw.txt,
#      ref/model_emerald.txt: 600 frames each) and is the same binary
#      (ref/perf.md5)
#  (2) the game is blind to the mixer: the picture hashes of the audio build
#      equal those of the no-audio build, every scene, every mixer setting
#  (3) recordings: the hash of the audio the mixer produced equals the
#      reference (ref/SCENE.SETTING.txt; ref/tone.txt for the synthetic
#      tone-channel test)
#  (4) the unit tests of the mixer (tone_test's checks, tests/audio/unit_*.sh)
#
#   tests/audio/run.sh [--rebase] [--rebase-model] [--scenes "aw emerald"]
#
# --rebase rewrites the references of (3): only where a task says the output
# may change, and the commit message must say why. --rebase-model rewrites
# those of (1) (only for a deliberate change of the default build).
# Time: about 4 minutes on an idle machine (the five scenes run in full:
# 600, 1900, 2400, 2400 and 2400 frames, twice, under plain qemu-arm; the
# model runs only aw and emerald, 600 frames).
. "$(dirname "$0")/lib.sh"
rebase=0; rebase_model=0; scenes=$SCENE_NAMES
while [ $# -gt 0 ]; do
  case $1 in
    --rebase) rebase=1 ;;
    --rebase-model) rebase_model=1 ;;
    --scenes) scenes=$2; shift ;;
    *) echo "usage: run.sh [--rebase] [--rebase-model] [--scenes \"aw emerald\"]"; exit 2 ;;
  esac
  shift
done
fail=0
pass() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }
S=$(mktemp -d "${TMPDIR:-/tmp}/audiogate.XXXXXX")
trap 'rm -rf "$S"' EXIT

build_harness || { bad "build of the speed harnesses"; exit 1; }
build_tone || { bad "build of the tone test"; exit 1; }

# ---- (1) the default build is untouched ----
sum=$(md5sum "$ARM/build/perf" | cut -d' ' -f1)
if [ $rebase_model = 1 ]; then echo "$sum" >"$REF/perf.md5"; fi
if [ "$sum" = "$(cat "$REF/perf.md5")" ]; then pass "(1) default speed harness binary identical to the recorded one"
else bad "(1) default speed harness binary differs from the recorded one ($sum)"; fi
for s in aw emerald; do
  model_ref_text gate$s $s 600 >"$S/model_$s.txt" 2>&1
  if [ $rebase_model = 1 ]; then cp "$S/model_$s.txt" "$REF/model_$s.txt"; fi
  if cmp -s "$S/model_$s.txt" "$REF/model_$s.txt"; then
    pass "(1) no-audio model numbers, $s 600 frames: $(grep '^  game' "$S/model_$s.txt" | awk '{print $2}') ms game, $(sed -n 's/^cycles \([0-9]*\).*/\1/p' "$S/model_$s.txt") cycles"
  else
    bad "(1) no-audio model numbers, $s 600 frames, differ from ref/model_$s.txt:"
    diff "$REF/model_$s.txt" "$S/model_$s.txt" | head -12
  fi
done

# ---- (2) pictures equal, (3) audio equals its reference ----
for s in $scenes; do
  scene_get "$s" || { bad "scene $s: game or state missing"; continue; }
  perf_functional perf "$s" "$SC_FRAMES" every=25 | grep '^frame' >"$S/$s.pic0"
  [ -s "$S/$s.pic0" ] || { bad "(2) scene $s: the no-audio build printed nothing"; continue; }
  while read -r name args; do
    perf_functional perf_audio "$s" "$SC_FRAMES" every=25 $args >"$S/$s.$name.out"
    grep '^frame' "$S/$s.$name.out" >"$S/$s.$name.pic"
    if cmp -s "$S/$s.pic0" "$S/$s.$name.pic"; then
      pass "(2) $s [$name]: $(wc -l <"$S/$s.pic0") picture hashes equal with the mixer on"
    else
      bad "(2) $s [$name]: pictures differ from the no-audio build"
      diff "$S/$s.pic0" "$S/$s.$name.pic" | head -6
    fi
    grep '^audio' "$S/$s.$name.out" >"$S/$s.$name.aud"
    if [ $rebase = 1 ]; then cp "$S/$s.$name.aud" "$REF/$s.$name.txt"; fi
    if [ -f "$REF/$s.$name.txt" ] && cmp -s "$S/$s.$name.aud" "$REF/$s.$name.txt"; then
      pass "(3) $s [$name]: audio hash $(tail -1 "$S/$s.$name.aud" | awk '{print $3" ("$4" samples)"}') equals the reference"
    else
      bad "(3) $s [$name]: audio differs from ref/$s.$name.txt (last: $(tail -1 "$S/$s.$name.aud"))"
      [ -f "$REF/$s.$name.txt" ] && diff "$REF/$s.$name.txt" "$S/$s.$name.aud" | head -6
    fi
  done < <(mixer_settings)
done

# ---- (3) the tone test, (4) the unit tests ----
tone_status=0
(cd "$ARM" && "$QEMU" -cpu arm926 build/audio/tone_test) >"$S/tone.out" 2>&1 || tone_status=$?
if [ $rebase = 1 ]; then grep -v '^PASS\|^FAIL' "$S/tone.out" >"$REF/tone.txt"; fi
if grep -v '^PASS\|^FAIL' "$S/tone.out" | cmp -s - "$REF/tone.txt"; then
  pass "(3) tone-channel test: output equals ref/tone.txt ($(grep '^tone hash' "$S/tone.out"))"
else
  bad "(3) tone-channel test output differs from ref/tone.txt"
  grep -v '^PASS\|^FAIL' "$S/tone.out" | diff "$REF/tone.txt" - | head -10
fi
if [ $tone_status = 0 ]; then pass "(4) tone-channel test: $(grep '^PASS' "$S/tone.out")"
else bad "(4) tone-channel test: $(grep '^FAIL' "$S/tone.out" | head -5)"; fi
for u in "$AUDIO_DIR"/unit_*.sh; do
  [ -f "$u" ] || continue
  if "$u" >"$S/unit.out" 2>&1; then pass "(4) $(basename "$u"): $(tail -1 "$S/unit.out")"
  else bad "(4) $(basename "$u"): $(tail -3 "$S/unit.out")"; fi
done

[ $fail = 0 ] && echo "audio gate: all checks passed" || echo "audio gate: FAILED"
exit $fail
