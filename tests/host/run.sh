#!/bin/sh
# Builds and runs the PC tests: the core and the frontend's parts
# (core_tests.c, frontend_tests.c), then whole scenarios on a simulated
# calculator (host_tests.py, host_hw.c), and with a USB gamepad
# (pad_tests.py, host_usbpad.c), and the seamless USB port's scenario matrix
# (seamless_tests.py: S9). Needs gcc, g++, zlib and python3,
# and jsmolka's GBA test ROMs (github.com/jsmolka/gba-tests) in $GBA_TESTS
# (no default).
# os_tests: nspire_os.c's own exit code (the hand-back) on a register file.
# SAN=1 ./run.sh: the same with AddressSanitizer and UBSan (alignment
# included) in build-san (make SAN=1); any report stops that test.
# GPSP_TEST_JOBS=N (default half the CPUs): the build and the tests below N
# at a time (par.py: each suite's tests too), each one's output printed whole
# and in the order listed; 1: one after another, stopping at the first that
# fails.
set -e
cd "$(dirname "$0")"
GBA_TESTS=${GBA_TESTS:?set GBA_TESTS to a copy of github.com/jsmolka/gba-tests}
JOBS=${GPSP_TEST_JOBS:-$(($(nproc) / 2))}
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
if [ -n "$SAN" ]; then
    export GPSP_TEST_BUILD=build-san
    make -s -j"$JOBS" SAN=1
else
    export GPSP_TEST_BUILD=build
    make -s -j"$JOBS"
fi
# suite COMMAND...: a test below. One at a time: run now; else run by par.py
# at the end, beside the others (a word a tab, a command a line).
PLAN=$GPSP_TEST_BUILD/par_plan.txt
: > "$PLAN"
suite() {
    if [ "$JOBS" -le 1 ]; then
        "$@"
    else
        printf '%s\t' "$@" >> "$PLAN"
        echo >> "$PLAN"
    fi
}
WORK=$GPSP_TEST_BUILD/work
rm -rf "$WORK"
mkdir -p "$WORK"
cp "$GBA_TESTS/arm/arm.gba" "$WORK/arm.gba.tns"
cp "$GBA_TESTS/save/sram.gba" "$WORK/sram.gba.tns"

# An 8 MB ROM: the ARM test, then text-like, random and padding bytes, with
# an odd size so the last page is partly past the end.
python3 -I - "$WORK" <<'EOF'
import random, sys
random.seed(7)
work = sys.argv[1]
rom = bytearray(open(work + "/arm.gba.tns", "rb").read())
while len(rom) < 3 * 1024 * 1024:
    rom += bytes(random.choice(b"ABCDEFGH ") for _ in range(4096))
rom += random.randbytes(2 * 1024 * 1024)
rom += b"\xff" * (8 * 1024 * 1024 - len(rom) - 1000)
open(work + "/big.gba.tns", "wb").write(rom)
EOF
python3 -I ../../tools/gbz.py pack -o "$WORK" "$WORK/arm.gba.tns" "$WORK/big.gba.tns" > /dev/null

# Zipped ROMs as zip tools write them (nspire_zip.c): deflated with a text
# file before the ROM in a folder, stored, a .agb, a ROM-less zip whose only
# big file is a .bin (refused: ZIP-4), with no ROM at all, damaged, and the 8 MB one.
python3 -I - "$WORK" <<'EOF'
import sys, zipfile
work = sys.argv[1]
arm = open(work + "/arm.gba.tns", "rb").read()
big = open(work + "/big.gba.tns", "rb").read()
D, S = zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED
def make(name, files, method=D):
    with zipfile.ZipFile(work + "/" + name, "w", method) as z:
        for inner, data in files:
            z.writestr(inner, data)
make("arm.zip.tns", [("readme.txt", b"read me\n" * 40), ("roms/arm.gba", arm)])
make("stored.zip.tns", [("arm.gba", arm)], S)
make("agb.zip.tns", [("game.agb", arm)])
make("noext.zip.tns", [("notes.txt", b"x" * 500), ("game.bin", arm)])
make("text.zip.tns", [("a.txt", b"only text")])
make("big.zip.tns", [("big.gba", big)])
data = bytearray(open(work + "/arm.zip.tns", "rb").read())
i = data.index(b"roms/arm.gba") + len("roms/arm.gba") + 2000   # inside the ROM's deflated data
data[i] ^= 0xFF
open(work + "/bad.zip.tns", "wb").write(bytes(data))
EOF
# (both in $WORK: one after the other)
suite sh -ec "./$GPSP_TEST_BUILD/core_tests '$WORK'; ./$GPSP_TEST_BUILD/frontend_tests '$WORK'"
suite ./$GPSP_TEST_BUILD/os_tests
suite ./$GPSP_TEST_BUILD/memmap_tests
suite ./$GPSP_TEST_BUILD/file_replace_tests
# R11u: the drive driver's attach never refuses after its match (built only
# with nspire-usbms there)
if [ -x ./$GPSP_TEST_BUILD/drive_os_tests ]; then
    suite ./$GPSP_TEST_BUILD/drive_os_tests
else
    suite echo "drive_os_tests: skipped (no nspire-usbms)"
fi
suite python3 -I host_tests.py
# the quit's save with states_serve_os 0, from the menu too
suite sh serve_os_test.sh
suite python3 -I pad_tests.py
suite python3 -I seamless_tests.py
suite python3 -I diag_tests.py
# 2026-11-16: a USB hub on the port (a controller, sound and a drive at once,
# devices moved between its ports, no power restart under it), and the OS's
# USB found stopped (hub_tests.py; host_usbpad.c's "pad hub", "pad wedge").
suite python3 -I hub_tests.py
# Bluetooth (bt_tests.py; host_bt.c's "bt ..." lines): no dongle costs
# nothing, the pads' slots, the sound's routing, the page, the settings, the exit.
suite python3 -I bt_tests.py
# R10b: the pad glue with the real nspire-usbpad on its fake TI USB stack
# (tests/padsim; skipped without the driver's tests/os_fake).
if [ -n "$SAN" ]; then
    suite make -s -f ../padsim/padsim.mk SAN=1 BUILD=build-san-padsim padsim-run
else
    suite make -s -f ../padsim/padsim.mk BUILD=build-padsim padsim-run
fi
# The real mixer and USB audio driver (nspire_audio_os.c's build of
# nspire-usbaudio's ua_os.c) on that driver's fake TI USB stack, a
# PlayStation pad's audio interfaces left to the pad driver included
# (2026-11-15; skipped without $USB_AUDIO_DIR's tests/fake). The driver:
# ../nspire-usbaudio-gpsp when there (the worktree kept for gpSP), as the
# top Makefile and sink.mk. pace_test: the frame loop's OS time, the
# sound's rate and the frameskip while sound plays, 60 s of model time a
# case (tests/audio/pace_test.c).
if [ -z "$USB_AUDIO_DIR" ] && [ -f ../../../nspire-usbaudio-gpsp/src/ua_os.h ]; then
    UA=../../../nspire-usbaudio-gpsp
else
    UA=${USB_AUDIO_DIR:-../../../nspire-usbaudio}
fi
if [ -f "$UA/tests/fake/fake_os.c" ] && [ -f "$UA/src/ua_os.c" ]; then
    if [ -n "$SAN" ]; then SINK=build-sink-san; else SINK=build-sink; fi
    suite sh -ec "make -s -f ../audio/sink.mk BUILD=$SINK SAN=1 USB_AUDIO_DIR='$UA'; \
        make -s -f ../audio/sink_boundaries.mk BUILD=$SINK SAN=1 USB_AUDIO_DIR='$UA'; \
        make -s -f ../audio/pace.mk BUILD=$SINK SAN=1 USB_AUDIO_DIR='$UA'; \
        make -s -f ../audio/mix.mk BUILD=$SINK SAN=1 USB_AUDIO_DIR='$UA'; \
        ./$SINK/sink_test; ./$SINK/sink_boundaries_test; ./$SINK/pace_test; ./$SINK/mix_test"
else
    suite echo "sink tests: skipped (no $UA/tests/fake/fake_os.c)"
fi
suite sh ../build_id_test.sh
suite sh ../usbpad_dir_test.sh
[ "$JOBS" -le 1 ] || python3 -I par.py --jobs "$JOBS" --plan "$PLAN"
