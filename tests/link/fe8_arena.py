#!/usr/bin/env python3
"""Makes a Fire Emblem: The Sacred Stones save with a Link Arena team, for
the link tests: on the title's Extras, Link Arena is there (a save with a
team turns it on) and its Link battle line too, without playing a chapter.

    fe8_arena.py OUT.sav[.gz].tns [--team NAME]   a new save, one team (LINK)
    fe8_arena.py --rankings SAVE                   prints the rankings: NAME:POINTS

The save is the game's own new save (32 KB of SRAM: the header the game
writes when it finds none, its records' zeros) and the Link Arena's block
(save slot 5): ten teams of five units, the rules, the rankings, under its
32-bit checksum. The team is Eirika, Seth, Gilliam, Franz and Vanessa at
level 10 with their weapons.

The layout is the decomp's (FireEmblemUniverse/fireemblem8u: bmsave.h
struct SaveBlocks, MultiArenaSaveBlock, GameSavePackedUnit; bmsave-lib.c
Checksum16, ReadSaveBlockInfo; asm/arm.s Checksum32)."""
import gzip, struct, sys

SRAM = 0x8000
ARENA, ARENA_SIZE = 0x691C, 0x874        # gSram->multiArenaBlock
TEAM_SIZE, UNIT_SIZE = 0xC4, 0x24        # name[15], 5 units from 0x10
INFO = 0x64                              # saveBlockInfo[7], 16 bytes each

# GameSavePackedUnit's bit fields (LSB first, straddling words), then pid,
# weapon ranks (sword, lance, axe, bow, staff, anima, light, dark), supports.
FIELDS = [("class", 7), ("level", 5), ("exp", 7), ("x", 6), ("y", 6), ("flag", 13), ("hp", 6),
          ("pow", 5), ("skl", 5), ("spd", 5), ("def", 5), ("res", 5), ("lck", 5), ("con", 5), ("mov", 5),
          ("item1", 14), ("item2", 14), ("item3", 14), ("item4", 14), ("item5", 14)]

def item(number, uses):
    return number | uses << 8

TEAM = [
    # person, class, HP, pow, skl, spd, def, res, lck, items, ranks
    (0x01, 0x02, 30, 10, 14, 16, 8, 8, 12, [item(0x09, 40), item(0x6C, 3)], {0: 71}),    # Eirika, Rapier
    (0x02, 0x07, 40, 16, 16, 14, 12, 10, 10, [item(0x14, 45), item(0x01, 46)], {0: 121, 1: 121}),  # Seth
    (0x03, 0x09, 32, 12, 8, 6, 16, 4, 6, [item(0x14, 45)], {1: 71}),                     # Gilliam
    (0x04, 0x05, 28, 10, 10, 11, 8, 3, 8, [item(0x01, 46), item(0x14, 45)], {0: 31, 1: 31}),  # Franz
    (0x06, 0x48, 24, 8, 12, 16, 6, 8, 10, [item(0x15, 30)], {1: 71}),                    # Vanessa
]

# The rankings a new save starts with (bmsave-multiarena.c
# gInitialMultiArenaRankings): rank, players, mode, points, name.
RANKINGS = [(0, 1, 0, 100, "Lord"), (0, 1, 1, 90, "Sniper"), (0, 1, 0, 80, "Shaman"),
            (0, 2, 1, 70, "Cavalier"), (0, 2, 0, 60, "Fighter"), (1, 2, 1, 50, "Warrior"),
            (1, 3, 0, 40, "Knight"), (1, 3, 1, 30, "General"), (1, 3, 0, 20, "Archer"),
            (1, 3, 1, 10, "Druid")]

def checksum16(data):
    """The header's: the halfwords' sum plus their xor."""
    words = struct.unpack("<%dH" % (len(data) // 2), data)
    x = 0
    for w in words:
        x ^= w
    return (sum(words) + x) & 0xFFFF

def checksum32(data):
    """A save block's: the halfwords' sum (low half) and xor (high half)."""
    words = struct.unpack("<%dH" % (len(data) // 2), data)
    x = 0
    for w in words:
        x ^= w
    return (sum(words) & 0xFFFF) | x << 16

def unit(person, job, hp, pow_, skl, spd, def_, res, lck, items, ranks):
    values = {"class": job, "level": 10, "hp": hp, "pow": pow_, "skl": skl, "spd": spd, "def": def_,
              "res": res, "lck": lck}
    for i, it in enumerate(items):
        values["item%d" % (i + 1)] = it
    bits, at = 0, 0
    for name, width in FIELDS:
        bits |= (values.get(name, 0) & ((1 << width) - 1)) << at
        at += width
    out = bytearray(bits.to_bytes(20, "little")) + bytes([person])
    out += bytes(ranks.get(i, 0) for i in range(8)) + bytes(7)
    return bytes(out)

def new_save(team_name):
    save = bytearray(b"\xff" * SRAM)
    # The header (GlobalSaveInfo): "AGB-FE9", magics, nothing played yet.
    head = bytearray(0x64)
    head[0:8] = b"AGB-FE9\0"
    struct.pack_into("<IH", head, 8, 0x40624, 0x200A)
    struct.pack_into("<H", head, 0x60, checksum16(head[:0x50]))
    save[0:0x64] = head
    # Records, sound room, bonus claims: as a new save has them.
    save[0x7190:0x73A0] = bytes(0x73A0 - 0x7190)
    for off, value in ((0x7224, 6), (0x722C, 8), (0x7244, 0x1C)):
        save[off] = value

    arena = bytearray(ARENA_SIZE)
    name = team_name.encode("ascii")[:14]
    arena[0:len(name)] = name
    for i, u in enumerate(TEAM):
        arena[0x10 + i * UNIT_SIZE:0x10 + (i + 1) * UNIT_SIZE] = unit(*u)
    # Rules: show units, survival, auto equip off (a new save's).
    struct.pack_into("<H", arena, 10 * TEAM_SIZE, 7)
    for i, (rank, players, mode, points, who) in enumerate(RANKINGS):
        at = 10 * TEAM_SIZE + 4 + i * 0x14
        struct.pack_into("<I", arena, at, rank | players << 2 | mode << 4 | points << 5)
        arena[at + 4:at + 4 + len(who)] = who.encode("ascii")
    save[ARENA:ARENA + ARENA_SIZE] = arena
    # Its block info (slot 5): magics, kind 2 (arena), where, size, sum.
    struct.pack_into("<IHBbHHI", save, INFO + 5 * 16, 0x20112, 0x200A, 2, -1, ARENA, ARENA_SIZE,
                     checksum32(arena))
    return save

def rankings(path):
    """The rankings' names and points, best first (a battle's winner goes
    in when it beats one: sio_bat.c sub_8045A64)."""
    save = open(path, "rb").read()
    if save[:2] == b"\x1f\x8b":
        save = gzip.decompress(save)
    out = []
    for i in range(10):
        at = ARENA + 10 * TEAM_SIZE + 4 + i * 0x14
        points = struct.unpack_from("<I", save, at)[0] >> 5
        out.append("%s:%d" % (save[at + 4:at + 19].split(b"\0")[0].decode("ascii", "replace"), points))
    return out

def main():
    args = sys.argv[1:]
    if args[0] == "--rankings":
        print(" ".join(rankings(args[1])))
        return
    team = "LINK"
    if "--team" in args:
        i = args.index("--team"); team = args[i + 1]; del args[i:i + 2]
    save = bytes(new_save(team))
    open(args[0], "wb").write(gzip.compress(save) if args[0].endswith(".gz.tns") else save)

main()
