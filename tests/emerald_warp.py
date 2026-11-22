#!/usr/bin/env python3
"""An Emerald (USA) battery save that continues somewhere else.

    python3 -I tests/emerald_warp.py IN.sav[.gz][.tns] OUT.sav GROUP NUM X Y [stage=N] [repel=N] [nocall=1]

Sets the newest save slot's continue-game warp (SaveBlock1 +0x0C: map group,
number, warp id -1, x, y) and SaveBlock2's CONTINUE_GAME_WARP flag (+0x09
bit 0; pokeemerald src/load_save.c, include/save_location.h), and fixes the
sectors' checksums (src/save.c: the 32-bit sum of the sector's data, folded
to 16 bits). "Continue" on the title's menu then loads the saved map and
warps into GROUP/NUM at X, Y the way the game warps itself (CB2_ContinueSavedGame:
the new map's header, its weather, its OnTransition script, the player
surfing on water or diving on an underwater map). stage=N sets SaveBlock1's
weatherCycleStage (+0x2F: Route 119's cycle 0 sun, 1 rain, 2 thunderstorm,
3 rain); repel=N a Repel's steps left (VAR_REPEL_STEP_COUNT 0x4021, vars at
+0x139C: no wild Pokemon below the lead's level for N steps); nocall=1
clears FLAG_HAS_MATCH_CALL (0x12F, flags at +0x1270: the PokeNav's trainer
calls, whose first comes within a few steps of a continue). Used to make the effect scenes' states (tests/emerald_fx_states.sh).
"""
import gzip
import struct
import sys

SECTOR, SLOTS = 4096, 14
SIZES = {0: 0xF2C, 1: 0xF80, 2: 0xF80, 3: 0xF80, 4: 0xF08}   # SaveBlock2, SaveBlock1's four
SB1_PART = 0xF80                                               # SaveBlock1's bytes in a sector


def checksum(data):
    s = sum(struct.unpack('<%dI' % (len(data) // 4), data)) & 0xFFFFFFFF
    return ((s >> 16) + s) & 0xFFFF


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__)
    opts = dict(a.split('=', 1) for a in sys.argv[7:])
    raw = open(sys.argv[1], 'rb').read()
    if raw[:2] == b'\x1f\x8b':
        raw = gzip.decompress(raw)
    d = bytearray(raw)
    group, num, x, y = (int(a) for a in sys.argv[3:7])
    slots = []
    for slot in range(2):
        counters = [struct.unpack_from('<I', d, (slot * SLOTS + i) * SECTOR + 0xFFC)[0] for i in range(SLOTS)]
        sigs = [struct.unpack_from('<I', d, (slot * SLOTS + i) * SECTOR + 0xFF8)[0] for i in range(SLOTS)]
        if all(s == 0x08012025 for s in sigs):
            slots.append((counters[0], slot))
    if not slots:
        sys.exit('no valid save slot')
    slot = max(slots)[1]
    where = {}
    for i in range(SLOTS):
        base = (slot * SLOTS + i) * SECTOR
        where[struct.unpack_from('<H', d, base + 0xFF4)[0]] = base
    sb2, sb1 = where[0], where[1]
    struct.pack_into('<bbbbhh', d, sb1 + 0x0C, group, num, -1, 0, x, y)
    d[sb2 + 0x09] |= 1
    if 'stage' in opts:
        d[sb1 + 0x2F] = int(opts['stage'])
    if 'repel' in opts:
        off = 0x139C + 2 * 0x21
        struct.pack_into('<H', d, where[1 + off // SB1_PART] + off % SB1_PART, int(opts['repel']))
    if opts.get('nocall') == '1':
        off = 0x1270 + 0x12F // 8
        d[where[1 + off // SB1_PART] + off % SB1_PART] &= ~(1 << (0x12F % 8)) & 0xFF
    for sid in (0, 1, 2, 3, 4):
        base = where[sid]
        struct.pack_into('<H', d, base + 0xFF6, checksum(bytes(d[base:base + SIZES[sid]])))
    open(sys.argv[2], 'wb').write(bytes(d))


if __name__ == '__main__':
    main()
