#!/bin/sh
# The drive driver's tests (drive_tests.c) on a FAT16 and a FAT32 image, which
# are also the images the other drive suites use (build/drive-work/fat16.img,
# fat32.img: drive_ui_tests.py, drive_save_tests.py, ui_wired.py's drive screens):
#   drive_tests.sh            the images, then both runs side by side
#   drive_tests.sh --images   only the images (GAMES/, Test.gba, the two .img)
#   drive_tests.sh --run 16   one run (16 or 32) on an image already made
# Each run has its own copy of documents/ (it writes saves there) and its own
# checksum file, so the two don't touch each other.
set -eu
cd "$(dirname "$0")"
USBMS_DIR=${USBMS_DIR:-../../../nspire-usbms}
BUILD=${GPSP_TEST_BUILD:-build}
work="$BUILD/drive-work"

images() {
make -s USBMS_DIR="$USBMS_DIR" ${SAN:+SAN=1} "$BUILD/drive_tests"
mkdir -p "$work/GAMES/Folder" "$work/documents"
python3 -I rom/make_test_rom.py "$work/pattern.gba"
python3 -I - "$work" <<'PY'
import pathlib, random, sys, zipfile
w = pathlib.Path(sys.argv[1])
rom = bytearray((w / 'pattern.gba').read_bytes())
r = random.Random(37)
rom += r.randbytes(8 * 1024 * 1024 - len(rom) - 1000)
(w / 'GAMES/Test.gba').write_bytes(rom)
(w / 'GAMES/Long name with spaces.gba').write_bytes(rom)
with zipfile.ZipFile(w / 'GAMES/Test.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('Test.gba', rom)
# Same name and size on the calculator, different contents. A USB selection
# must never be replaced by an alleged already-copied file.
rom[-1] ^= 0xff
(w / 'documents/Test.gba').write_bytes(rom)
(w / 'GAMES/notes.txt').write_text('not a game')
PY
python3 -I ../../tools/gbz.py pack "$work/GAMES/Test.gba" >/dev/null
for bits in 16 32; do
  img="$work/fat$bits.img"
  truncate -s 96M "$img"
  if [ "$bits" = 16 ]; then spc=4; else spc=1; fi
  mkfs.vfat -F "$bits" -s "$spc" "$img" >/dev/null
  MTOOLS_SKIP_CHECK=1 mcopy -s -i "$img" "$work/GAMES" ::/
done
}

# run BITS: the test on that image, and the image as it was after
run() {
  img="$work/fat$1.img"
  docs="$work/documents$1"
  mkdir -p "$docs"
  cp "$work/documents/Test.gba" "$docs/Test.gba"
  sha256sum "$img" > "$work/before$1.sha256"
  "$BUILD/drive_tests" "$(realpath "$img")" "$(realpath "$work/GAMES/Test.gba")" "$(realpath "$docs")"
  sha256sum -c "$work/before$1.sha256"
}

case "${1:-}" in
--images) images ;;
--run) run "$2" ;;
"")
  images
  # side by side, each one's output printed whole, the 16 first
  ( run 16 > "$work/run16.out" 2>&1 ) & p16=$!
  ( run 32 > "$work/run32.out" 2>&1 ) & p32=$!
  rc=0
  wait $p16 || rc=1
  cat "$work/run16.out"
  wait $p32 || rc=1
  cat "$work/run32.out"
  exit $rc ;;
*) echo "usage: drive_tests.sh [--images | --run 16|32]" >&2; exit 2 ;;
esac
