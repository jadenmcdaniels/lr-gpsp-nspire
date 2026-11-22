"""Pokemon Emerald: trades over the link cable (the Cable Club) and the
wireless adapter (the Direct Corner), on CX IIs and original CXs with either
as player 1; the same rooms' trade screen held for 10 s (soak_*, the link's
steady state: the speed matrix's); linking from the menus; and the hello
choosing the cable as wired when one side requested it.

Both start from the reference save, put at Rustboro's Pokemon Center 2F desk
by emerald_desk.py (player 2's second Pokemon named LINKMON). A trade
passes when both games saved it: player 1 has player 2's LINKMON, player 2
has player 1's CP3 in its place. On the cable both use Link cable (exact),
whatever stand-in Emerald gets."""
import os, subprocess
from linklib import Scenario, wait, press, presses

ROM = "Pokemon - Emerald Version (USA, Europe).gba"
SAVE_NAME = "Pokemon - Emerald Version (USA, Europe).sav.gz.tns"
SAVE = os.environ.get("EMERALD_SAVE", "")
DESK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "emerald_desk.py")
ALL = (0, 1, 3, 10)

BOOT = wait(1) + ["link"] + wait(20) + press("shift") + wait(3) + press("shift") + wait(3) + \
    press("del") + wait(1.5) + press("del") + wait(4) + press("8") + wait(1)


def at_desk(desk):
    def prepare(player, saves):
        cmd = ["python3", "-I", DESK, SAVE, os.path.join(saves, SAVE_NAME), "--desk", desk]
        subprocess.run(cmd + (["--name", "LINKMON"] if player == 2 else []), check=True)
    return prepare


def sit(direction):
    """From the Trade Center's door to the chair on that side (one press is
    one step): up to the table, one step aside, one up onto the chair; the
    trade screen comes."""
    return wait(4) + ["hold 8"] + wait(1.3) + ["release 8"] + wait(0.5) + press(str(direction)) + \
        wait(0.5) + press("8") + wait(9)


def trade(slot2):
    """At the trade screen: pick the first Pokemon (or the second), TRADE,
    YES, and the trade and its save. YES twice: the question comes once
    both have picked (a press more on "Communication standby" does
    nothing)."""
    pick = (press("6") + wait(0.5)) if slot2 else wait(0.5)
    return pick + press("del") + wait(1.5) + press("5") + wait(0.5) + press("del") + wait(5) + \
        press("del") + wait(2) + press("del") + wait(20) + ["shot t20"] + wait(20) + ["shot t40"] + \
        wait(20) + ["shot end"]


def soak():
    """10 s at the trade screen, both seated: the link's idle traffic."""
    return ["measure"] + wait(10) + ["shot soak", "measure stop"] + wait(1)


def cable_scripts(end, settle=0, late_link=False):
    # Cable Club (the right desk without an adapter): its welcome, Trade
    # Center, save; player 1 (the parent) confirms the 2 players; into the
    # room; the parent's chair is on the left. settle (R3): seconds more
    # before walking to the chair, for a link delay of 15 frames, at which
    # the desk's "Please wait..." handshake takes longer in frames.
    def scripts():
        common = BOOT + presses("del", 10, 1.5)
        if late_link:
            # R3: Link picked once both games are at the desk's "Please wait"
            # (as on the calculators), not at the start.
            common = [c for c in BOOT if c != "link"] + presses("del", 10, 1.5) + ["link"] + wait(5)
        p1 = common + press("del") + wait(2) + presses("del", 8, 2) + wait(settle) + sit(4) + ["shot seat"] + end(False)
        p2 = common + wait(2.6) + presses("del", 8, 2) + wait(settle) + sit(6) + ["shot seat"] + end(True)
        return p1, p2
    return scripts


def wireless_scripts(end):
    # Direct Corner: its welcome, Trade Center, save; player 2 becomes the
    # group's leader, player 1 joins it and player 2 accepts; the leader is
    # the parent, so its chair is the left one.
    def scripts():
        common = BOOT + presses("del", 7, 1.5) + wait(6) + press("del") + wait(2)
        p1 = common + press("del") + wait(4) + press("del") + wait(3) + presses("del", 8, 2) + sit(6) + \
            ["shot seat"] + end(False)
        p2 = common + press("5") + wait(0.5) + press("del") + wait(6.5) + presses("del", 8, 2) + sit(4) + \
            ["shot seat"] + end(True)
        return p1, p2
    return scripts


def party(player):
    out = subprocess.run(["python3", "-I", DESK, "--party", os.path.join(player.saves, SAVE_NAME)],
                         capture_output=True, text=True)
    return out.stdout.split()


def traded(run):
    p1, p2 = party(run.players[0]), party(run.players[1])
    ok = p1[:1] == ["LINKMON"] and p2[1:2] == ["CP3"] and "LINKMON" not in p2 and \
        all(p.linked() for p in run.players)
    return ok, "player 1: %-16s player 2: %s" % (" ".join(p1), " ".join(p2))


def soaked(run):
    """Both linked through the 10 s, the games talking (cable transfers, or
    the adapter's packets: more than 16 bytes a message)."""
    ok = True
    for p in run.players:
        frames = p.number_of("link_frames")
        if run.scenario.devices[0] == "wireless":
            talk = p.number_of("link_bytes_received") > 16 * p.number_of("link_messages_received")
        else:
            talk = p.number_of("link_transfers") > frames
        ok = ok and frames >= 590 and talk
    return ok, "linked frames %s / %s" % (run.players[0].report.get("link_frames"),
                                          run.players[1].report.get("link_frames"))


def statuses(run):
    return [p.report.get("link", "-") for p in run.players]


def linked_from_menu(run):
    s = statuses(run)
    on_line = all("ui=main:3" in p.notes.get("line", "") for p in run.players)
    ok = on_line and s[0].startswith("Linked: player 1") and s[1].startswith("Linked: player 2")
    return ok, "player 1: %s / player 2: %s%s" % (s[0], s[1], "" if on_line else " (Link line not picked)")


def menu_scripts():
    # Both open the menu and pick Link (the fourth line); the games go on
    # linked, player 1's to the cable's parent.
    script = ["wait 3", "press menu", "wait 0.5"] + ["press down", "wait 0.15"] * 3 + \
        ["shot line", "press enter", "wait 3", "shot linked"]
    return script, script


def fell_back(run):
    """Player 1's cable is exact, player 2's Emerald's (Pokemon) stand-in:
    both play it as wired, and player 2 says why (the status once linked:
    player 2 then ends with player 1 gone, as on the exact cable it's
    behind)."""
    s = [p.report.get("link_linked", "-") for p in run.players]
    ok = s == ["Linked: player 1 (Link cable, exact)",
               "Linked: player 2 (Link cable, exact): the other GBA is set to exact"]
    return ok, "player 1: %s / player 2: %s" % (s[0], s[1])


def stood_in(run):
    """Link cable takes the game's stand-in (Auto gives Emerald the adapter;
    frontend_tests checks Auto's cable)."""
    s = statuses(run)
    ok = s == ["Linked: player 1 (Link cable, quick)", "Linked: player 2 (Link cable, quick)"]
    return ok, "player 1: %s / player 2: %s" % (s[0], s[1])


def hello():
    return wait(1) + ["link"] + wait(2), wait(1) + ["link"] + wait(2)


def scenarios():
    needs = ["gba_bios.bin", SAVE]
    cable, wireless = at_desk("cable"), at_desk("direct")
    out = [
        Scenario("menu", ROM, menu_scripts, linked_from_menu, quick=True),
        Scenario("exact_quick", ROM, hello, fell_back, devices=("exact", "cable"),
                 args=((), ("--stand-in", "BPEE:poke")), quick=True),
        Scenario("stand_in", ROM, hello, stood_in, devices=("cable", "cable"),
                 args=(("--stand-in", "BPEE:poke"),) * 2, quick=True),
    ]
    for m1, m2, delays in (("cx2", "cx2", (3,)), ("cx2", "cx", ()), ("cx", "cx2", ())):
        out.append(Scenario("cable_%s_%s" % (m1, m2), ROM, cable_scripts(trade), traded, models=(m1, m2),
                            devices=("exact", "exact"), prepare=cable, needs=needs, delays=delays, quick=True))
    out.append(Scenario("wireless_cx2_cx", ROM, wireless_scripts(trade), traded, models=("cx2", "cx"),
                        devices=("wireless", "wireless"), prepare=wireless, needs=needs, quick=True))
    for m1, m2 in (("cx2", "cx2"), ("cx2", "cx")):
        out.append(Scenario("soak_cable_%s_%s" % (m1, m2), ROM, cable_scripts(lambda slot2: soak()), soaked,
                            models=(m1, m2), devices=("exact", "exact"), prepare=cable, needs=needs,
                            delays=ALL))
    out.append(Scenario("soak_wireless_cx2_cx", ROM, wireless_scripts(lambda slot2: soak()), soaked,
                        models=("cx2", "cx"), devices=("wireless", "wireless"), prepare=wireless,
                        needs=needs, delays=ALL))
    return out
