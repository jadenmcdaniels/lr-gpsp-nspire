#!/bin/sh
# Builds the battery probe (needs the Ndless toolchain on PATH): battery_probe.tns (read only) and
# battery_probe_w.tns (also starts each CX ADC channel's measurement). Not run by anything.
set -e
cd "$(dirname "$0")"
command -v nspire-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
for v in "" _w; do
  flags=
  [ "$v" = _w ] && flags=-DPROBE_WRITE
  nspire-gcc -Os -std=gnu99 -Wall -Wextra -march=armv5te -mtune=arm926ej-s $flags battery_probe.c -o battery_probe$v.elf
  genzehn --input battery_probe$v.elf --output battery_probe$v.tns --name battery_probe$v --version 12 \
          --author "gpSP" --clickpad-support=false --color-support=true --240x320-support=true \
          --uses-lcd-blit false --compress --ndless-min 31 --ndless-rev-min 2001
done
ls -l battery_probe.tns battery_probe_w.tns
