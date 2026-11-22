#!/bin/bash
# The same register sequence must serialize identically with a PCM mixer and
# with no host audio code at all. This caught the legacy frozen-PSG stub.
set -eu
. "$(dirname "$0")/lib.sh"
S=$(mktemp -d "${TMPDIR:-/tmp}/soundstate.XXXXXX")
trap 'rm -rf "$S"' EXIT
make -s -C "$ARM" -j4 -f ../audio/tone.mk build/sound_state_test >"$S/build.log" 2>&1 || { cat "$S/build.log"; exit 1; }
make -s -C "$ARM" -j4 -f ../audio/tone.mk AUDIO=1 build/audio/sound_state_test >"$S/build.log" 2>&1 || { cat "$S/build.log"; exit 1; }
"$QEMU" -cpu arm926 "$ARM/build/sound_state_test" >"$S/off"
"$QEMU" -cpu arm926 "$ARM/build/audio/sound_state_test" >"$S/on"
diff -u "$S/off" "$S/on"
[ "$(grep -c '^state ' "$S/on")" = 240 ]
echo 'PASS 240 complete sound states and status registers match with/without mixing'
