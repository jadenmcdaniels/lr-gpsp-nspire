"""Golden Sun: a linked battle in the Battle Arena (the title's Battle), on
the cable as wired (exact) and with its stand-in (quick: serial_msio.c,
which Auto gives it); and the arena's link idling for 10 s (gs_soak_*: the
link's steady state, for the speed matrix).

Both start from a new game's save with a Djinn (goldensun_save.py), player
2's leader renamed Links. In the arena player 1 applies for a battle at the
left counter, player 2 accepts, both step into the circle and fight, A on
every question (Fight, Attack, the enemy). Isaac against Isaac, the same
moves: player 2 wins in the fifth turn. The battle passes when both games
saved its result: player 2's file a win (0x4EC; its streak, 0x4F0 and
0x4F2, too), player 1's a loss (0x4EE)."""
import os, subprocess
from linklib import Scenario, wait, press, frames
import goldensun_save

ROM = "Golden Sun (USA, Europe).gba"
SAVE_NAME = "Golden Sun (USA, Europe).sav.tns"
MAKE_SAVE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "goldensun_save.py")
ALL = (0, 1, 3, 10)


def prepare(player, saves):
    cmd = ["python3", "-I", MAKE_SAVE, os.path.join(saves, SAVE_NAME)]
    subprocess.run(cmd + (["--name", "Links"] if player == 2 else []), check=True)


# The title's Battle (two left of Continue), the arena's welcome, and up to
# the left counter's girl. Golden Sun's link runs from the arena's door on.
BOOT = wait(1) + ["link"] + wait(5) + press("shift") + wait(1.75) + press("shift") + wait(2.75) + \
    press("4") + wait(0.25) + press("4") + wait(0.25) + press("del") + wait(1.75) + press("del") + \
    wait(3.75) + press("del") + wait(1.75) + press("del") + wait(1.75) + press("del") + wait(1.75) + \
    press("del") + wait(0.75) + ["hold 8"] + wait(0.7) + ["release 8"] + wait(0.3) + \
    ["hold 4"] + wait(1.2) + ["release 4"] + wait(0.3) + ["hold 8"] + wait(0.4) + ["release 8"] + wait(0.3)


def talk(n):
    return sum((press("del") + wait(1.5) for _ in range(n)), [])


# From the counter into the circle.
CIRCLE = ["hold 6"] + frames(71) + ["release 6"] + wait(0.3) + ["hold 8"] + frames(36) + ["release 8"]


def battle_scripts():
    # Player 1 applies (6 lines), player 2 is asked a little later and
    # accepts (5); both in the circle by the same frame, then A every 1.5 s
    # (40 times: the battle, its results saved, and the arena).
    p1 = BOOT + talk(6) + wait(5.5) + press("var") + wait(0.5) + CIRCLE + wait(8)
    p2 = BOOT + wait(8) + talk(5) + CIRCLE + wait(8)
    end = ["measure"] + talk(40) + ["shot end", "measure stop"]
    return p1 + end + wait(1), p2 + end


def soak_scripts():
    # Both at the counter, the link idling.
    p1 = BOOT + ["measure"] + wait(10) + ["shot soak", "measure stop"] + wait(1)
    p2 = BOOT + ["measure"] + wait(10) + ["shot soak", "measure stop"]
    return p1, p2


def record(player):
    """Its save's link battles (won, lost): gpSP gzips the save at the end."""
    gz = os.path.join(player.saves, SAVE_NAME[:-4] + ".gz.tns")
    return goldensun_save.record(gz if os.path.exists(gz) else os.path.join(player.saves, SAVE_NAME))


def battled(run):
    (w1, l1), (w2, l2) = (record(p) for p in run.players)
    ok = (w1, l1, w2, l2) == (0, 1, 1, 0) and all(p.linked() for p in run.players)
    return ok, "player 1: %d won %d lost, player 2: %d won %d lost" % (w1, l1, w2, l2)


def soaked(run):
    """Both linked through the 10 s, the games talking (more than a frame's
    own message's bytes)."""
    ok = True
    for p in run.players:
        n = p.number_of("link_frames")
        ok = ok and n >= 590 and p.number_of("link_bytes_received") > 16 * n
    return ok, "linked frames %s / %s" % (run.players[0].report.get("link_frames"),
                                          run.players[1].report.get("link_frames"))


def soaked_as_wired(run):
    """Player 1 on Auto (the stand-in), player 2 on Link cable (exact), as
    a game without one (The Lost Age) would be: both play it as wired
    (transfers, which the stand-in makes none of)."""
    ok, what = soaked(run)
    t = [p.number_of("link_transfers") for p in run.players]
    ok = ok and all(n > p.number_of("link_frames") for n, p in zip(t, run.players))
    return ok, "%s, transfers %d / %d" % (what, t[0], t[1])


def scenarios():
    needs = ["gba_bios.bin"]
    out = []
    for name, device, delays in (("exact", "exact", (0, 3)), ("quick", "auto", (0, 3, 10))):
        out.append(Scenario("gs_battle_" + name, ROM, battle_scripts, battled, devices=(device, device),
                            prepare=prepare, needs=needs, delays=delays))
    for name, device, models in (("exact", "exact", ("cx2", "cx2")), ("quick", "auto", ("cx2", "cx2")),
                                 ("quick_cx2_cx", "auto", ("cx2", "cx"))):
        out.append(Scenario("gs_soak_" + name, ROM, soak_scripts, soaked, models=models,
                            devices=(device, device), prepare=prepare, needs=needs, delays=ALL))
    out.append(Scenario("gs_soak_auto_exact", ROM, soak_scripts, soaked_as_wired, devices=("auto", "exact"),
                        prepare=prepare, needs=needs, delays=(0,)))
    return out
