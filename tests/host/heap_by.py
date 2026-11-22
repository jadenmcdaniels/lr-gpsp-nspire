#!/usr/bin/env python3
"""save17: a gpsp-host heapdump (OUTDIR/heap_NAME.txt: an offset from
host_heap_dump, bytes, allocations) by function and line, the biggest first.

    heap_by.py OUTDIR/heap_NAME.txt [N]     (build/gpsp-host, or $GPSP_TEST_BUILD's)"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, os.environ.get("GPSP_TEST_BUILD", "build"), "gpsp-host")


def main():
    rows = [line.split() for line in open(sys.argv[1])]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    nm = subprocess.run(["nm", HOST], capture_output=True, text=True).stdout
    base = next(int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(" host_heap_dump"))
    rows = rows[:top]
    addrs = ["%x" % ((base + int(off, 16)) & 0xffffffffffffffff) for off, _, _ in rows]
    where = subprocess.run(["addr2line", "-f", "-s", "-e", HOST] + addrs, capture_output=True, text=True).stdout.split("\n")
    for i, (off, size, n) in enumerate(rows):
        print("%10s bytes %6s allocations  %s %s" % (size, n, where[2 * i], where[2 * i + 1]))


if __name__ == "__main__":
    main()
