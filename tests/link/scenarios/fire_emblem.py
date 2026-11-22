"""Fire Emblem: The Sacred Stones: a Link Arena battle over the link cable,
with Fire Emblem's stand-in (serial_fe.c, what Auto and Link cable give the
game) and as wired (Link cable (exact)); and the arena's lobby held for 10 s
(fe_soak_*, the speed matrix's).

Both start from a new save with one Link Arena team (fe8_arena.py: player
1's PLAYER1, player 2's PLAYER2), so the title's Extras has Link Arena.
Both pick Linked Battle and their team, player 2 20 s after player 1 for a
battle (player 1 waits in the lobby alone meanwhile); once the two are in,
player 1 (the parent) starts the battle, the game draws who moves first,
and that player surrenders (START, Yes): the battle is over, and the
winner's team, 30 points for each of its five units, goes first in its
save's rankings. A battle passes when one save ranks its own team first
with 150 points and the other's rankings are as they were.

The exact cable's battle also writes both players' --link-trace (every
transfer) and checks what the stand-in counts on: the two GBAs agree on
every transfer, the parent starts at most two a frame (timer 3: 136 x 1024
cycles after each), and between the packets (0x4FFF, length, two sums, the
data) there's only 0x7FFF (nothing to say), the hello's two words, a
packet dropped after its 0x4FFF, and player 2's 0xFFFF (not there yet) and
0x0000 (its first word)."""
import os, subprocess
from collections import Counter
from linklib import Scenario, wait, press, presses

ROM = "Fire Emblem - The Sacred Stones (USA, Australia).gba"
SAVE_NAME = "Fire Emblem - The Sacred Stones (USA, Australia).sav.gz.tns"
ARENA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fe8_arena.py")
ALL = (0, 1, 3, 10)
FRESH = ["Lord:100", "Sniper:90", "Shaman:80", "Cavalier:70", "Fighter:60", "Warrior:50", "Knight:40",
         "General:30", "Archer:20", "Druid:10"]
LATE = 20   # s: player 2 comes to the lobby after player 1 for a battle
# Between packets: idle, the hello's (~0x6584, then 0x2586), a dropped
# packet's start, nothing on the wire, a GBA's first word.
BETWEEN = {0x7FFF, 0x9A7B, 0x2586, 0x4FFF, 0xFFFF, 0x0000}

# The health warning, logos, the intro and the title to its menu; Extras,
# Link Arena; Linked Battle, Team, the team: the two link up.
LINK = wait(1) + ["link"]
TO_LOBBY = wait(4) + presses("shift", 5, 2) + press("5") + wait(1) + press("del") + wait(2) + \
    press("del") + wait(3) + presses("5", 2, 1) + presses("del", 3, 1.5) + wait(6)
LOBBY = LINK + TO_LOBBY


def team(player, saves):
    subprocess.run(["python3", "-I", ARENA, os.path.join(saves, SAVE_NAME), "--team", "PLAYER%d" % player],
                   check=True)


def battle():
    """Player 2 comes LATE s after player 1, who starts it once both are
    in; the draw, the rules, the map; both press START, and whoever moves
    first gets "Do you surrender?" (on Yes) and says yes (the other's
    presses do nothing); the units go, the results; on from them (the
    winner's new ranking, which saves it)."""
    after = wait(30) + press("shift") + wait(3) + press("del") + wait(30) + ["shot results"] + \
        press("del") + wait(6) + press("del") + wait(3) + ["shot end"]
    return (LOBBY + wait(LATE) + press("shift") + after,
            LINK + wait(LATE) + TO_LOBBY + wait(0.25) + after + wait(1))


def soak():
    """10 s in the lobby, both in: the beacons and their answers. Each
    stays a while after (player 1 longest), so neither's window loses the
    other's last frames."""
    window = LOBBY + ["measure"] + wait(10) + ["shot soak", "measure stop"]
    return window + wait(1), window + wait(0.5)


def rankings(player):
    out = subprocess.run(["python3", "-I", ARENA, "--rankings", os.path.join(player.saves, SAVE_NAME)],
                         capture_output=True, text=True)
    return out.stdout.split()


def won(run):
    r = [rankings(p) for p in run.players]
    winners = [n for n in (1, 2) if r[n - 1][:1] == ["PLAYER%d:150" % n] and r[n - 1][1:] == FRESH[:9]]
    ok = len(winners) == 1 and r[2 - winners[0]] == FRESH and all(p.linked() for p in run.players)
    return ok, "player %s won; player 1: %s / player 2: %s" % (
        winners[0] if len(winners) == 1 else "?", " ".join(r[0][:2]), " ".join(r[1][:2]))


def transfers(player):
    """Its --link-trace: (player 1's frame, cycle, player 1's word, player
    2's) for each transfer (the SIOCNT column left out: each GBA's own)."""
    with open(os.path.join(player.dir, "trace.txt")) as f:
        return [(int(a), int(b), int(c, 16), int(d, 16)) for a, b, _, c, d in (l.split() for l in f)]


def packets(words):
    """A GBA's words on the wire: how many packets (0x4FFF, a length up to
    0x80, two sums, that many words) and the other words."""
    count, other, i = 0, Counter(), 0
    while i < len(words):
        if words[i] == 0x4FFF and i + 1 < len(words) and words[i + 1] <= 0x80:
            count += 1
            i += 4 + words[i + 1]
        else:
            other[words[i]] += 1
            i += 1
    return count, other


def traced(run):
    """The battle, and its transfers as the stand-in takes them."""
    ok, what = won(run)
    t = [transfers(p) for p in run.players]
    cycles = [x[1] for x in t[0]]
    gap = min(b - a for a, b in zip(cycles, cycles[1:]))
    most = max(Counter(x[0] for x in t[0]).values())
    ok = ok and t[0] == t[1] and len(t[0]) == run.players[0].number_of("link_transfers") and \
        gap >= 136 * 1024 and most <= 2
    what += "; %d transfers, %d cycles apart or more, %d a frame at most" % (len(t[0]), gap, most)
    for n in (1, 2):
        count, other = packets([x[1 + n] for x in t[0]])
        ok = ok and set(other) <= BETWEEN
        what += "; player %d: %d packets, between them %s" % (
            n, count, " ".join("%04X:%d" % w for w in sorted(other.items())))
    return ok, what


def soaked(run):
    """Both linked through the 10 s, the games talking: cable transfers,
    or the stand-in's packets (more than the frames' 8-byte heads)."""
    ok = True
    for p in run.players:
        frames = p.number_of("link_frames")
        if run.scenario.devices[0] == "exact":
            talk = p.number_of("link_transfers") > frames
        else:
            talk = p.number_of("link_bytes_received") > 8 * p.number_of("link_messages_received")
        ok = ok and frames >= 590 and talk
    return ok, "linked frames %s / %s" % (run.players[0].report.get("link_frames"),
                                          run.players[1].report.get("link_frames"))


def scenarios():
    needs = ["gba_bios.bin"]
    trace = (("--link-trace", "p1/trace.txt"), ("--link-trace", "p2/trace.txt"))
    out = [
        Scenario("fe_arena_cx2_cx", ROM, battle, won, models=("cx2", "cx"), prepare=team, needs=needs,
                 delays=(0, 3, 10), quick=True),
        Scenario("fe_arena_exact_cx2_cx2", ROM, battle, traced, devices=("exact", "exact"), prepare=team,
                 needs=needs, args=trace, delays=(3,), quick=True),
        Scenario("fe_soak_cx2_cx", ROM, soak, soaked, models=("cx2", "cx"), prepare=team, needs=needs,
                 delays=ALL),
    ]
    for m1, m2 in (("cx2", "cx2"), ("cx2", "cx")):
        out.append(Scenario("fe_soak_exact_%s_%s" % (m1, m2), ROM, soak, soaked, models=(m1, m2),
                            devices=("exact", "exact"), prepare=team, needs=needs, delays=ALL))
    return out
