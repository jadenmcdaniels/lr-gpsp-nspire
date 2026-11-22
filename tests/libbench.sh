#!/bin/bash
# What gpSP's load and save paths cost on the calculator, library code
# (zlib, newlib's stdio and memcpy) included: ROMs (.gba, .zip, .gbz),
# states, in-game saves, the ROM manager, the settings, the file list,
# starting and leaving. See tests/libbench.py.
#   tests/libbench.sh LABEL [BASELINE.txt]
# writes build/perf/libbench-LABEL.txt; with BASELINE (an earlier one) each
# time gets a delta column. Needs the Ndless toolchain (on PATH, or NSPIRE_ENV,
# done here if needed), the plugin-enabled qemu-arm (QEMU=, as
# tests/calc_speed.sh) and the games and states in $GBA_GAMES.
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
exec python3 -I "$(dirname "$0")/libbench.py" "$@"
