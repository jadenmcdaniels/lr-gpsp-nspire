#!/bin/bash
# Estimates how fast gpSP runs a game on the calculator, from a PC: see
# tests/calc_speed.py (or run with --help).
#   tests/calc_speed.sh [STATE] [--frames N] [--draw N] [--top N] ...
# Needs the Ndless toolchain (on PATH, or NSPIRE_ENV: a script sourced here)
# and a qemu-arm built with plugins (QEMU=: its build directory, with
# include/qemu beside it, as configure --enable-plugins leaves it).
command -v arm-none-eabi-gcc >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
exec python3 -I "$(dirname "$0")/calc_speed.py" "$@"
