"""Advance Wars: a two-player Multi-Pak battle over the link cable, played as
wired (Link cable, exact) and with libretro's stand-in (serial_proto.c's
serialaw: Auto's and Link cable's quick cable, nspire_link.c's stand_ins).

Both start a new game (the reference in-game save is empty: they play from
states) and go to Link Mode's Multi-Pak; player 1 (the parent) starts,
picks the first map (Brace Range, pre-deployed, fog on) and its CO, player
2 its CO, player 1 the rules as they are. Then four turns, two days: each
moves an infantry (player 2's captures a city) and its recon towards the
other's; on Day 2 player 1's recon crosses the river, and player 2's
attacks it (and is hit back: the damage has luck in it). It passes when
both GBAs have the same battle at Day 3's start, read from a state each
saves then (S): the day, whose turn it is, both armies' units (where, HP,
ammo, fuel, capturing) with the moves and the fight in them."""
import os, struct, zlib
from linklib import Scenario, wait, press, presses, frames

ROM = "Advance Wars (USA) (Rev 1).gba"
STATE = "Advance Wars (USA) (Rev 1)0.svs.tns"   # slot 1's state
DELAYS = (0, 3, 10)

# The game's RAM (US Rev 1): the day (IWRAM), the army whose turn it is
# (0x20 Orange Star, player 1's; 0x40 Blue Moon, player 2's), and each
# army's units (EWRAM, 64 of 12 bytes: type, x, y, HP + ammo << 7, fuel).
DAY, ARMY = 0x03004420, 0x03004424
UNITS = (0x02019F40, 0x0201A240)
UNIT, SLOTS = 12, 64
# Where the four turns leave the units that move: (army, unit): x, y. The
# first infantries and the recons (the sixth units).
MOVED = {(0, 0): (1, 4), (1, 0): (11, 2), (0, 5): (7, 6), (1, 5): (7, 7)}


def costs(model):
    """Advance Wars' own emulation: ~6 ms a frame on a CX II, not
    Emerald's 4 (the speed table, name entry); three times on a CX."""
    return ["cost frame %d" % (6 if model == "cx2" else 18)]


def keys(names, gap=0.6):
    return sum((press(k) + wait(gap) for k in names), [])


def length(lines):
    return sum(int(l.split()[1]) for l in lines if l.startswith("frames "))


def steps(*parts):
    """Each part is (player, lines): that player's lines while the other
    waits as long, or (0, lines) for both."""
    p1, p2 = [], []
    for player, lines in parts:
        idle = frames(length(lines))
        p1, p2 = p1 + (idle if player == 2 else lines), p2 + (idle if player == 1 else lines)
    return p1, p2


def unit(to, path, command=("del",), after=0):
    """A unit's move: the cursor keys to it, A, the path, A, the walk, then
    its command (A: the menu's first line: Wait, Capture, Fire)."""
    return keys(to + ["del"] + path + ["del"]) + wait(1.5) + keys(list(command)) + wait(after)


# A on an empty tile or a unit that's done: the map menu; End is its last
# line. The cursor starts each turn on the HQ.
END = keys(["del", "8", "del"]) + wait(6)

# From power on to Multi-Pak's lobby, linked from the first second: the
# title (Start twice), Nell's welcome, the name A (Start, Yes), not the first
# time playing (right, A), the talk after it; Link Mode (up from Field Training),
# Multi-Pak.
BOOT = wait(1) + ["link"] + wait(15) + presses("shift", 2, 2) + presses("del", 8, 1) + press("shift") + \
    wait(1) + presses("del", 5, 1.5) + press("6") + wait(0.5) + press("del") + wait(2) + \
    presses("del", 20, 1.5) + press("8") + wait(1) + presses("del", 2, 2)

# The lobby to Day 1: everyone's in (Start), Brace Range, player 1's CO,
# player 2's, the rules. The quick cable takes ~4 s more to the map list,
# and a press before it's there is lost.
LOBBY = ((1, press("shift") + wait(8) + press("del") + wait(4) + press("del") + wait(2)),
         (2, press("del") + wait(2)), (1, press("del") + wait(8)))

# Day 1: player 1's first infantry (L) up two over the bridge, its recon
# (four down) up the road and right to (3, 2); player 2's first infantry up
# two onto a city (Capture), its recon (three right) down its road to
# (11, 7). Day 2: player 1's recon (three right, six up from the HQ) right
# and down over the bridge to (7, 6); player 2's (the sixth L) left four,
# under it: Fire, at it (the only one in reach), the fight; End from its
# own recon (down one).
TURNS = ((1, unit(["scratch"], ["8", "8"]) + unit(["5"] * 4, ["8"] * 6 + ["6"] * 2) + END),
         (0, ["shot turn1"]),
         (2, unit(["scratch"], ["8", "8"], after=3) + unit(["6"] * 3, ["4"] + ["5"] * 5 + ["4"] * 2) + END),
         (0, ["shot turn2"]),
         (1, unit(["6"] * 3 + ["8"] * 6, ["6"] * 4 + ["5"] * 4) + END),
         (0, ["shot turn3"]),
         (2, unit(["scratch"] * 6, ["4"] * 4, ("del", "del"), after=8) + keys(["5"]) + END))


def battle_scripts(models):
    def scripts():
        p1, p2 = steps(*(LOBBY + ((0, ["shot day1", "measure"]),) + TURNS +
                         ((0, ["shot day3", "measure stop"] + press("S") + wait(1)),)))
        return costs(models[0]) + BOOT + p1 + wait(1), costs(models[1]) + BOOT + p2
    return scripts


def state_ram(path):
    """IWRAM and EWRAM from a gpSP state (nspire_states.c: the header, then
    the state, zlib-packed unless flags has 2; in it, libretro's bson)."""
    with open(path, "rb") as f:
        data = f.read()
    magic, _, _, _, packed, flags = struct.unpack_from("<4s5I", data)
    if magic != b"GPSZ":
        raise ValueError("not a state: " + path)
    raw = data[32:32 + packed]
    raw = raw if flags & 2 else zlib.decompress(raw)
    ram = {}
    for name, base in (("iwram", 0x03000000), ("ewram", 0x02000000)):
        key = b"\x05" + name.encode() + b"\x00"
        at = raw.index(key) + len(key)
        ram[base] = raw[at + 5:at + 5 + struct.unpack_from("<i", raw, at)[0]]
    return ram


def battle(player):
    """The day, whose turn (1 or 2) and the two armies' units as saved."""
    ram = state_ram(os.path.join(player.saves, STATE))
    def at(address, n):
        base = address & 0xFF000000
        return ram[base][address - base:address - base + n]
    army = at(ARMY, 1)[0]
    units = [[at(u + i * UNIT, UNIT) for i in range(SLOTS)] for u in UNITS]
    return at(DAY, 1)[0], {0x20: 1, 0x40: 2}.get(army, army), [[r for r in a if any(r)] for a in units]


def hp(unit):
    return (unit[4] | unit[5] << 8) & 0x7F


def same_battle(run):
    """Day 3, player 1's turn on both GBAs, the units the same on both, and
    the moves and the fight in them: the units in MOVED there, the two
    recons hurt."""
    b1, b2 = battle(run.players[0]), battle(run.players[1])
    units = b1[2]
    moved = all(len(a) > 5 for a in units) and \
        all(tuple(units[a][u][2:4]) == xy for (a, u), xy in MOVED.items())
    fought = moved and hp(units[0][5]) < 100 and hp(units[1][5]) < 100
    ok = b1 == b2 and b1[:2] == (3, 1) and fought and all(p.linked() for p in run.players)
    return ok, "player 1: day %d, player %s's turn, recons %s; player 2: day %d, player %s's turn, %s%s" % (
        b1[0], b1[1], "/".join(str(hp(a[5])) for a in units if len(a) > 5) or "-", b2[0], b2[1],
        "the same units" if b1[2] == b2[2] else "other units",
        "" if fought else " (the moves and the fight aren't all in)")


def scenarios():
    out = []
    for name, models, devices, delays, quick in (
            ("aw_exact_cx2_cx2", ("cx2", "cx2"), ("exact", "exact"), DELAYS, False),
            ("aw_quick_cx2_cx2", ("cx2", "cx2"), ("auto", "auto"), DELAYS, True),
            ("aw_quick_cx2_cx", ("cx2", "cx"), ("auto", "auto"), (3,), False)):
        out.append(Scenario(name, ROM, battle_scripts(models), same_battle, models=models, devices=devices,
                            needs=["gba_bios.bin"], delays=delays, quick=quick))
    return out
