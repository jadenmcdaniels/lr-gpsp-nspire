#!/bin/bash
# The pacing model (pace_test.c, 2026-11-15): the frame loop's OS time, the
# sound's rate and the frameskip while sound plays, cases a-e of 60 s of
# model time each against the audio driver's fake TI USB stack (the driver
# worktree as tests/host/run.sh finds it). The last line: each case's game
# frames a second, silent packets and wire gaps.
set -eu
. "$(dirname "$0")/lib.sh"
HOST=$ROOT/tests/host
S=$(mktemp -d "${TMPDIR:-/tmp}/soundpace.XXXXXX")
trap 'rm -rf "$S"' EXIT
# (USB_AUDIO_DIR first when set, as tests/host/run.sh: a worktree whose ../
# holds another driver checkout)
if [ -n "${USB_AUDIO_DIR:-}" ]; then UA=$USB_AUDIO_DIR
elif [ -f "$ROOT/../nspire-usbaudio-gpsp/src/ua_os.h" ]; then UA=$ROOT/../nspire-usbaudio-gpsp
else UA=$ROOT/../nspire-usbaudio; fi
if [ ! -f "$UA/tests/fake/fake_os.c" ]; then
  echo "PASS pacing model: skipped (no $UA/tests/fake/fake_os.c)"
  exit 0
fi
make -s -C "$HOST" -j4 -f ../audio/pace.mk BUILD=build-sink-pace SAN=1 USB_AUDIO_DIR="$UA" >"$S/build.log" 2>&1 || {
  cat "$S/build.log"; exit 1;
}
"$HOST/build-sink-pace/pace_test" >"$S/out" 2>&1 || { cat "$S/out"; exit 1; }
cat "$S/out" >&2
# (speed9a) where the OS tick's timer isn't read (an unknown set-up: r15's
# rule), the pad's ticks are the glue's: the frame loop's runs the same
"$HOST/build-sink-pace/pace_test" --case pad --no-timer >"$S/out2" 2>&1 || { cat "$S/out2"; exit 1; }
cat "$S/out2" >&2
summary=$(sed -n 's/^pace \([^ ]*\) *game \([0-9.]*\) fps, pictures \([0-9.]*\);.*silent packets \([0-9]*\) .*wire gaps \([0-9]*\) ms.*delay max \([0-9]*\) ms.*/\1 \2\/\3 fps, silent \4, gaps \5, delay <= \6 ms/p' "$S/out" | paste -sd ';' | sed 's/;/; /g')
echo "PASS pacing model ($(tail -1 "$S/out" | sed 's/.*: //'); without the tick's timer $(tail -1 "$S/out2" | sed 's/.*: //')): $summary"
