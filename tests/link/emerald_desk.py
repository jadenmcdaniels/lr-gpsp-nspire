#!/usr/bin/env python3
"""Puts a Pokemon Emerald save at a Pokemon Center 2F link desk, for the link
tests: on Continue the player stands at Rustboro's 2F (in front of the Direct
Corner desk by default), with at least two Pokemon (the first is copied).

    emerald_desk.py IN.sav[.gz][.tns] OUT.sav.gz.tns [--desk cable|direct|union|guide] [--name NAME]
    emerald_desk.py --party SAVE      prints the party's nicknames

--name gives the second Pokemon a nickname, to tell the traded one apart (the
nickname isn't part of the checksummed, encrypted data).

The save layout and map numbers are pokeemerald's (pret/pokeemerald: save.h,
global.h SaveBlock1/2, save_location.h, data/maps)."""
import gzip, struct, sys

SECTOR = 0x1000
DATA = 3968
# Rustboro City Pokemon Center 2F (group 11, map 6); the desks are at y=2:
# the wireless club's guide, the Union Room's, and the right one, the
# Direct Corner with a wireless adapter and the Cable Club without.
DESKS = {"guide": 2, "union": 6, "direct": 10, "cable": 10}

# Gen 3 text: A-Z, a-z, 0-9; 0xFF ends a name.
def encode(name):
    out = bytearray()
    for c in name:
        if "A" <= c <= "Z": out.append(0xBB + ord(c) - 65)
        elif "a" <= c <= "z": out.append(0xD5 + ord(c) - 97)
        elif "0" <= c <= "9": out.append(0xA1 + ord(c) - 48)
        else: out.append(0)
    return (bytes(out) + b"\xff" * 10)[:10]

def decode(raw):
    out = ""
    for b in raw:
        if b == 0xFF: break
        if 0xBB <= b <= 0xD4: out += chr(65 + b - 0xBB)
        elif 0xD5 <= b <= 0xEE: out += chr(97 + b - 0xD5)
        elif 0xA1 <= b <= 0xAA: out += chr(48 + b - 0xA1)
        else: out += "?"
    return out

def checksum(data):
    s = sum(struct.unpack("<%dI" % (len(data) // 4), data)) & 0xFFFFFFFF
    return ((s >> 16) + s) & 0xFFFF

def load(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return bytearray(raw)

def newest_slot(save):
    def sectors(slot):
        out = {}
        for n in range(14):
            off = (slot * 14 + n) * SECTOR
            sid, _, sig, counter = struct.unpack_from("<HHII", save, off + 0xFF4)
            if sig == 0x08012025:
                out[sid] = (off, counter)
        return out
    slots = [sectors(0), sectors(1)]
    return max((s for s in slots if 0 in s and 1 in s), key=lambda s: s[0][1])

def party(path):
    save = load(path)
    off = newest_slot(save)[1][0]
    count = save[off + 0x234]
    return [decode(save[off + 0x238 + 100 * i + 8:off + 0x238 + 100 * i + 18]) for i in range(count)]

def main():
    args = sys.argv[1:]
    if args[0] == "--party":
        print(" ".join(party(args[1])))
        return
    desk = "direct"
    name = None
    if "--name" in args:
        i = args.index("--name"); name = args[i + 1]; del args[i:i + 2]
    if "--desk" in args:
        i = args.index("--desk"); desk = args[i + 1]; del args[i:i + 2]
    src, dst = args
    raw = open(src, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    save = bytearray(raw)

    # The newer of the two slots (14 sectors each): the higher counter.
    def sectors(slot):
        out = {}
        for n in range(14):
            off = (slot * 14 + n) * SECTOR
            sid, _, sig, counter = struct.unpack_from("<HHII", save, off + 0xFF4)
            if sig == 0x08012025:
                out[sid] = (off, counter)
        return out
    slots = [sectors(0), sectors(1)]
    slot = max((s for s in slots if 0 in s and 1 in s), key=lambda s: s[0][1])

    def edit(sid, change):
        off = slot[sid][0]
        old = struct.unpack_from("<H", save, off + 0xFF6)[0]
        # The checksummed length is the saveblock chunk's: find it from the
        # sum that's there.
        size = next(n for n in range(DATA, 0, -4) if checksum(save[off:off + n]) == old)
        data = bytearray(save[off:off + DATA])
        change(data)
        save[off:off + DATA] = data
        struct.pack_into("<H", save, off + 0xFF6, checksum(save[off:off + size]))

    def block2(d):
        d[0x09] |= 1                       # CONTINUE_GAME_WARP
    def block1(d):
        # continueGameWarp: group, map, warp none (-1), x, y (below the desk)
        struct.pack_into("<bbbxhh", d, 0x0C, 11, 6, -1, DESKS[desk], 3)
        if d[0x234] < 2:                   # playerPartyCount
            d[0x238 + 100:0x238 + 200] = d[0x238:0x238 + 100]
            d[0x234] = 2
        if name:
            d[0x238 + 100 + 8:0x238 + 100 + 18] = encode(name)
    def block1_part2(d):
        # vars[VAR_CABLE_CLUB_TUTORIAL_STATE - 0x4000] = 2: the attendant's
        # first-visit tour is over (vars at 0x139C, this sector from 0xF80)
        struct.pack_into("<H", d, 0x139C + 2 * 0xCD - DATA, 2)
    edit(0, block2)
    edit(1, block1)
    edit(2, block1_part2)
    open(dst, "wb").write(gzip.compress(bytes(save)))

main()
