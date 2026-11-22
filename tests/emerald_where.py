#!/usr/bin/env python3
"""Where an Emerald (USA) gpSP save state stands: the map of each state given.

    python3 -I tests/emerald_where.py STATE.svs.tns|FOLDER ...

Reads nspire_states.c's file (32-byte "GPSZ" header, zlib or stored), the
BSON "iwram" and "ewram" blobs (gba_memory.c), then SaveBlock1's location
(gSaveBlock1Ptr at 0x03005D8C; location.mapGroup / mapNum at +4 / +5;
pokeemerald's include/global.h and data/maps/map_groups.json). Used to pick
the speed hunt's scenes (tests/calc_speed.py SCENES).
"""
import struct
import sys
import zlib

TOWNS = ['Petalburg City', 'Slateport City', 'Mauville City', 'Rustboro City', 'Fortree City',
         'Lilycove City', 'Mossdeep City', 'Sootopolis City', 'Ever Grande City', 'Littleroot Town',
         'Oldale Town', 'Dewford Town', 'Lavaridge Town', 'Fallarbor Town', 'Verdanturf Town',
         'Pacifidlog Town']


def blob(raw, key):
    tag = b'\x05' + key + b'\x00'
    i = raw.find(tag)
    if i < 0:
        return None
    n = struct.unpack_from('<I', raw, i + len(tag))[0]
    start = i + len(tag) + 5   # length, subtype
    return raw[start:start + n]


def where(path):
    data = open(path, 'rb').read()
    if data[:4] != b'GPSZ':
        return 'not a GPSZ state'
    _, _, raw_size, packed, flags, _, _ = struct.unpack_from('<7I', data, 4)
    body = data[32:32 + packed]
    raw = body if flags & 4 and packed == raw_size else None
    if raw is None:
        try:
            raw = zlib.decompress(body)
        except zlib.error:
            raw = body
    iwram, ewram = blob(raw, b'iwram'), blob(raw, b'ewram')
    if not iwram or not ewram:
        return 'no iwram/ewram in the state'
    ptr = struct.unpack_from('<I', iwram, 0x5D8C)[0]
    if not 0x02000000 <= ptr < 0x02040000:
        return 'gSaveBlock1Ptr %08x: not Emerald (USA) in the overworld' % ptr
    group, num = ewram[ptr - 0x02000000 + 4], ewram[ptr - 0x02000000 + 5]
    name = ''
    if group == 0:
        name = TOWNS[num] if num < len(TOWNS) else 'Route %d' % (101 + num - len(TOWNS))
    return 'map group %d number %d %s' % (group, num, name)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    import os
    for arg in sys.argv[1:]:
        paths = [arg] if not os.path.isdir(arg) else sorted(
            (os.path.join(arg, f) for f in os.listdir(arg) if f.endswith('.svs.tns')),
            key=lambda p: (len(p), p))
        for path in paths:
            print('%s: %s' % (path.split('/')[-1], where(path)))


if __name__ == '__main__':
    main()
