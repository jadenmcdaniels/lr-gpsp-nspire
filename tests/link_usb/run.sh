#!/bin/sh
# nspire_link_usb.c on the PC, twice, through a fake NavNet (usb_test.c).
#   tests/link_usb/run.sh [-v]
#   SAN=1 tests/link_usb/run.sh: with AddressSanitizer and UBSan (alignment
#   included), in build-san; any report stops it.
set -e
cd "$(dirname "$0")"
B=build
F=
if [ -n "$SAN" ]; then
    B=build-san
    F="-fsanitize=address,undefined,alignment -fno-sanitize-recover=all -fno-omit-frame-pointer"
fi
mkdir -p $B
for s in a b; do
    gcc $F -O1 -g -Wall -Wextra -Wno-unused-parameter -DLINK_USB_TEST -DSIDE=$s -Iinclude -I../.. \
        -c ../../nspire_link_usb.c -o $B/usb_$s.o
    gcc $F -O1 -g -Wall -Wextra -Wno-unused-parameter -DLINK_USB_TEST -DSIDE=$s -Iinclude -I../.. \
        -c ../../nspire_link_fast.c -o $B/fast_$s.o
done
gcc $F -O1 -g -Wall -Wextra -pthread usb_test.c usb_fast_sim.c $B/usb_a.o $B/usb_b.o $B/fast_a.o $B/fast_b.o \
    -o $B/usb_test
./$B/usb_test "$@"
