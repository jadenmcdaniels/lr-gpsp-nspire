#!/bin/sh
# Every PC suite again with AddressSanitizer and UBSan (alignment included,
# -fno-sanitize-recover: the first report stops that test): what the
# calculator punishes (alignment exceptions are on: nspire/nspire.c, an
# unaligned load resets it) and a PC forgives. One command:
#   tests/san.sh            core, frontend (with the link fuzz), host, pad,
#                           diag, link_usb, and the link matrix
#   tests/san.sh quick      the same without the matrix
# Builds in tests/host/build-san and tests/link_usb/build-san; the matrix's
# runs go to tests/link/work-san. gpSP's own core (upstream) is built without
# the shift check (it shifts into the sign bit by design): tests/host/Makefile.
set -e
cd "$(dirname "$0")"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
SAN=1 ./host/run.sh
SAN=1 ./link_usb/run.sh
if [ "$1" != quick ]; then
    GPSP_TEST_BUILD=build-san python3 -I link/run.py --matrix
fi
echo "san: all passed"
