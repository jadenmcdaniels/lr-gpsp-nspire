"""Pokemon Gen 3 on the link cable with its stand-in (serial_proto.c's
serialpoke, in nspire_link.c's stand_ins table): Emerald's Cable Club trade
and its trade screen held for 10 s (soak), as emerald.py's cable_* and
soak_cable_* (which play the cable exact), and a Colosseum single battle's
first three turns, on CX IIs and a CX II with an original CX. Both on Link
cable: Auto gives Emerald the wireless adapter (Ruby and Sapphire get the
stand-in on Auto; frontend_tests checks)."""
import importlib.util, os, struct
from linklib import Scenario, wait, press, presses

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("link_scenarios_emerald_for_pokemon",
                                               os.path.join(HERE, "emerald.py"))
emerald = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(emerald)


def soaked(run):
    """Both linked through the 10 s, the stand-ins talking: a packet (24
    bytes and its length) in about every frame's message (8 bytes)."""
    ok = True
    for p in run.players:
        ok = ok and p.number_of("link_frames") >= 590 and \
            p.number_of("link_bytes_received") > 30 * p.number_of("link_messages_received")
    return ok, "linked frames %s / %s" % (run.players[0].report.get("link_frames"),
                                          run.players[1].report.get("link_frames"))


# Emerald's (USA, Europe) RAM (pret/pokeemerald's symbols): the two
# parties (struct Pokemon: level at 0x54, HP and its most at 0x56), the
# battle's kind (BATTLE_TYPE_LINK 2, _IS_MASTER 4).
PARTY, ENEMY, BATTLE_TYPE = 0x020244EC, 0x02024744, 0x02022FEC


def colosseum():
    """The Cable Club's Colosseum: at the desk COLOSSEUM, SINGLE BATTLE and
    the save; player 1 confirms the 2 players. In the room each walks to its
    spot (3 up, 3 aside: player 1 to the left, player 2 to the right once
    player 1 is there). The battle's start, then A, A (FIGHT, the first
    move: POUND) over and over for three turns; 10 s with no keys, both at
    "What will CP3 do?"; the two parties' first Pokemon and the battle's
    kind (gpsp-host's peek)."""
    common = emerald.BOOT + press("del") + wait(3) + press("del") + wait(1.5) + press("5") + wait(0.5) + \
        press("del") + wait(2) + press("del") + wait(1.5) + presses("del", 6, 1.5)
    steps = lambda key: (press(key) + wait(0.4)) * 3
    battle = ["measure"] + wait(16) + presses("del", 48, 0.75) + wait(10) + \
        ["shot battle", "peek party %x:100" % PARTY, "peek enemy %x:100" % ENEMY,
         "peek type %x:4" % BATTLE_TYPE, "measure stop"]
    p1 = common + press("del") + wait(2) + presses("del", 8, 2) + wait(4) + steps("8") + steps("4") + wait(10)
    p2 = common + wait(2.25) + presses("del", 8, 2) + wait(11.9) + steps("8") + steps("6") + wait(2.1)
    return p1 + battle + wait(1), p2 + battle


def battled(run):
    """The two in one link battle, player 1 the master, three turns in:
    each one's Pokemon's HP is what the other GBA has for its enemy's, and
    both are hurt and standing."""
    got = []
    for p in run.players:
        mon = lambda name: struct.unpack_from("<HH", open(os.path.join(p.out, name + ".bin"), "rb").read(), 0x56)
        kind = struct.unpack("<I", open(os.path.join(p.out, "type.bin"), "rb").read())[0]
        got.append((mon("party"), mon("enemy"), kind & 6))
    (own1, foe1, kind1), (own2, foe2, kind2) = got
    ok = own1 == foe2 and own2 == foe1 and all(0 < hp < most for hp, most in (own1, own2)) and \
        (kind1, kind2) == (6, 2)
    return ok, "HP: player 1's %d/%d, player 2's %d/%d (as %d/%d, %d/%d on the other)" % (
        own1 + own2 + foe2 + foe1)


def stale_scripts():
    """R4: player 1's game as a state saved during a link leaves it (pret's
    gWirelessCommType 1, gReceivedRemoteLinkPlayers 1: the other side reset
    at the Cable Club): set in RAM at the desk, saved (S) and loaded (L),
    then Link and the Cable Club as poke_trade_late's. Both stay at "Please
    wait": player 1's game opens the wireless adapter's link, not the
    cable's."""
    unlinked = [c for c in emerald.BOOT if c != "link"]
    p1 = unlinked + ["poke 30030fc:1", "poke 3003124:1"] + press("S") + wait(2) + press("L") + wait(2) + \
        ["link"] + wait(5) + presses("del", 10, 1.5) + wait(2) + presses("del", 8, 2) + wait(10)
    p2 = unlinked + wait(8.5) + ["link"] + wait(5) + presses("del", 10, 1.5) + wait(2.6) + presses("del", 8, 2) + \
        wait(10)
    return p1, p2


def stale_hang(run):
    """Player 1 said the state was saved mid-link, its game ran the adapter's
    code (Emerald's wireless 1 to the end) and player 2's cable link stood
    open waiting (SIOCNT 601f): the hang the calculators showed in R3 has
    this cause when a state carries it."""
    def text(p, name):
        path = os.path.join(p.saves, name)
        return open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    p1, p2 = run.players
    said = "this state was saved during a link" in text(p1, "gpsp_link.txt.tns")
    last1 = [l for l in text(p1, "gpsp_link_last.txt.tns").splitlines() if " sio " in l][-1:]
    last2 = [l for l in text(p2, "gpsp_link_last.txt.tns").splitlines() if " sio " in l][-1:]
    adapter = bool(last1) and "Emerald: wireless 1" in last1[0]
    waiting = bool(last2) and "sio 601f" in last2[0]
    return said and adapter and waiting, "said %s, player 1's last second %s, player 2's %s" % (
        said, (last1 or ["-"])[0][:90], (last2 or ["-"])[0][:90])


def traded_saving_linked(run):
    """emerald.traded, and both wrote the game's save while linked the
    R4 way (no heap: nspire_backup.c save_packed_linked) and logged it: the
    save that came back is that file."""
    ok, what = emerald.traded(run)
    said = []
    for p in run.players:
        path = os.path.join(p.saves, "gpsp_link.txt.tns")
        text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        said.append("game save: written" in text and "game save: writing its file now" in text)
    return ok and all(said), what + " (linked saves logged %s)" % said


def traded_after_fall(run):
    """The trade done, and both fell back from the fast transport and
    measured the round trip again (the delay raised mid-link)."""
    ok, what = emerald.traded(run)
    said = []
    for p in run.players:
        path = os.path.join(p.saves, "gpsp_link.txt.tns")
        text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        said.append("fast transport: up" in text and "fell back to NavNet" in text and
                    "round trips over NavNet after the fall back" in text)
    return ok and all(said), what + " (fast up, fell back, measured again: %s)" % said


def scenarios():
    needs = ["gba_bios.bin", emerald.SAVE]
    cable = emerald.at_desk("cable")
    out = []
    for m1, m2 in (("cx2", "cx2"), ("cx2", "cx")):
        out.append(Scenario("poke_trade_%s_%s" % (m1, m2), emerald.ROM, emerald.cable_scripts(emerald.trade),
                            emerald.traded, models=(m1, m2), devices=("cable", "cable"), prepare=cable,
                            needs=needs, delays=(0, 3, 10), quick=m2 == "cx"))
        out.append(Scenario("poke_soak_%s_%s" % (m1, m2), emerald.ROM,
                            emerald.cable_scripts(lambda slot2: emerald.soak()), soaked, models=(m1, m2),
                            devices=("cable", "cable"), prepare=cable, needs=needs, delays=emerald.ALL))
        out.append(Scenario("poke_battle_%s_%s" % (m1, m2), emerald.ROM, colosseum, battled, models=(m1, m2),
                            devices=("cable", "cable"), prepare=cable, needs=needs, delays=(0, 10)))
    # R3: the real calculators' link (R2 logs: round trips 100-500 ms, a
    # delay of 15 frames): the Cable Club's handshake and a trade at 250 ms
    # one way (500 ms round trips: the delay's 15 frames).
    out.append(Scenario("poke_trade_slow_cx2_cx", emerald.ROM, emerald.cable_scripts(lambda slot2: emerald.wait(15) + emerald.trade(slot2), 20),
                        emerald.traded, models=("cx2", "cx"), devices=("cable", "cable"), prepare=cable,
                        needs=needs, delays=(250,)))
    out.append(Scenario("poke_trade_late_cx_cx2", emerald.ROM,
                        emerald.cable_scripts(lambda slot2: emerald.wait(15) + emerald.trade(slot2), 20, True),
                        traded_saving_linked, models=("cx", "cx2"), devices=("cable", "cable"), prepare=cable,
                        needs=needs, delays=(250,)))
    # The fast transport on R2's NavNet timing: up at the link's start, then
    # falling back mid-trade (gpsp-host's model): the delay is measured
    # again and raised mid-link, and the trade completes.
    navnet = ["--link-os", "--link-write", "40", "--link-slow", "260", "3", "--link-fast-fall", "1500"]
    out.append(Scenario("poke_trade_fast_fall_cx2_cx", emerald.ROM,
                        emerald.cable_scripts(lambda slot2: emerald.wait(15) + emerald.trade(slot2), 20),
                        traded_after_fall, models=("cx2", "cx"), devices=("cable", "cable"), prepare=cable,
                        needs=needs, delays=(42,), args=(navnet + ["--link-fast", "host"], navnet + ["--link-fast", "device"])))
    out.append(Scenario("poke_stale_state", emerald.ROM, stale_scripts, stale_hang, models=("cx", "cx2"),
                        devices=("cable", "cable"), prepare=cable, needs=needs, delays=(0,)))
    return out
