#!/bin/sh
# The calculator Makefile refuses a gamepad driver without the match fix
# (2026-11-11): the glue compiles against older drivers
# too, so a build with the default USBPAD_DIR silently dropped the fix.
# `make -n nspire_build_id.h` parses the Makefile without building (its
# recipe isn't run under -n). Prints "usbpad_dir: N checks, F failed".
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
n=0 f=0
check() { n=$((n + 1)); if [ "$1" = "$2" ]; then :; else f=$((f + 1)); echo "FAIL: $3: got '$1' want '$2'"; fi; }
mk() { make -n -C "$here" "$@" nspire_build_id.h > "$t/out" 2>&1; echo $?; }
refused() { grep -c 'lacks the match fix' "$t/out"; }

mkdir -p "$t/old/src" "$t/match_only/src" "$t/new/src"
: > "$t/old/usbpad.h"; : > "$t/match_only/usbpad.h"; : > "$t/new/usbpad.h"
echo '#define USBPAD_OS_NO_ID (-6)' > "$t/old/src/backend_os.h"   # 650532d alone
printf '%s\n' '#define USBPAD_OS_NO_ID (-6)' 'enum { USBPAD_OS_J_UNREAD = 0, USBPAD_OS_J_CLAIMED };' \
    > "$t/match_only/src/backend_os.h"
cat "$t/match_only/src/backend_os.h" > "$t/new/src/backend_os.h"
echo '#define USBPAD_OS_HAS_SHUTDOWN_PENDING 1' >> "$t/new/src/backend_os.h"

check "$(mk USBPAD_DIR="$t/old")" 2 "driver without the fix: make fails"
check "$(refused)" 1 "driver without the fix: says why"
check "$(mk USBPAD_DIR="$t/match_only")" 2 "match fix without callback shutdown: refused"
check "$(grep -c 'lacks safe callback shutdown' "$t/out")" 1 "missing shutdown guard: says why"
check "$(mk USBPAD_DIR="$t/new")" 0 "driver with the fix: parses"
check "$(refused)" 0 "driver with the fix: no refusal"
check "$(mk USBPAD_DIR="$t/none")" 0 "no driver at all: builds without pads"
check "$(grep -c 'built without USB gamepads' "$t/out")" 1 "no driver at all: says so"
check "$(mk USBPAD_DIR="$t/old" USBPAD=0)" 0 "USBPAD=0: no check"
check "$(make -n -C "$here" USBPAD_DIR="$t/old" clean > /dev/null 2>&1; echo $?)" 0 "clean: no check"
# The sibling this worktree is built with must pass (when it is there).
if [ -f "$here/../nspire-usbpad-pad/usbpad.h" ]; then
    check "$(mk USBPAD_DIR="$here/../nspire-usbpad-pad")" 0 "../nspire-usbpad-pad: has the fix"
fi
echo "usbpad_dir: $n checks, $f failed"
[ "$f" = 0 ]
