#!/bin/bash
# Independent ARM926 parity, saved tone references, and runtime boundaries.
set -eu
. "$(dirname "$0")/lib.sh"
S=$(mktemp -d "${TMPDIR:-/tmp}/soundruntime.XXXXXX")
trap 'rm -rf "$S"' EXIT
build() {
  make -s -C "$ARM" -j4 -f ../audio/runtime.mk "$@" >"$S/build.log" 2>&1 || {
    cat "$S/build.log"; exit 1;
  }
}
build BUILD=build/runtime-off build/runtime-off/runtime_state_test
build AUDIO=1 BUILD=build/runtime-on build/runtime-on/runtime_state_test
build AUDIO=1 RUNTIME=1 BUILD=build/runtime-switch build/runtime-switch/runtime_state_test \
      build/runtime-switch/runtime_toggle_test build/runtime-switch/tone_test
run() { "$QEMU" -cpu arm926 "$ARM/build/$1" "${@:2}"; }
run runtime-off/runtime_state_test >"$S/off"
run runtime-on/runtime_state_test on >"$S/on"
run runtime-switch/runtime_state_test off >"$S/runtime-off"
run runtime-switch/runtime_state_test on >"$S/runtime-on"
diff -u "$S/off" "$S/runtime-off"
diff -u "$S/on" "$S/runtime-on"
[ "$(grep -c '^state ' "$S/off")" = 240 ]
run runtime-off/runtime_state_test toggle >"$S/canonical-off"
run runtime-on/runtime_state_test toggle >"$S/canonical-on"
run runtime-switch/runtime_state_test toggle >"$S/toggle"
diff -u "$S/canonical-off" "$S/toggle"
diff -u "$S/canonical-on" "$S/toggle"
run runtime-switch/tone_test >"$S/tone"
grep -v '^PASS\|^FAIL' "$S/tone" >"$S/tone-reference"
diff -u "$REF/tone.txt" "$S/tone-reference"
run runtime-switch/runtime_toggle_test
echo 'PASS runtime silent/enabled/toggling: 240 states each; saved tone output; mute/reset/load boundaries'
