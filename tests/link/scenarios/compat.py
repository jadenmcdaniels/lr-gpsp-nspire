"""Any gpSP links with any other: this build with e24876d's (the CX CAS's,
protocol 1) in both player orders, and two of this build whose settings or
games differ (nspire_link.c's hello; frontend_tests checks every pair of
settings and games, these play them).

With e24876d (hosts "legacy": its gpsp-host from its own worktree,
linklib.LEGACY_HOST; skipped without it): Emerald's Cable Club trade on the
cable as wired (this side Link cable (exact), e24876d's Link cable, which
is exact) and its Direct Corner trade on Auto (the adapter on both), and an
Advance Wars battle with this side on Auto (alone its stand-in) and
e24876d on Auto (the cable as wired): both play it as wired. The links
speak protocol 1 (link_protocol), at no delay (e24876d's socket carries no
times).

Two of this build: Emerald on Auto with Link cable (both the stand-in; the
Auto side had the adapter in since power-on, also linking only once in the
Pokemon Center), Auto with exact, Wireless with Link cable, exact with
Wireless, each a Cable Club trade; Advance Wars on Auto with exact; two
different games linked for 20 s (Emerald and Golden Sun, Advance Wars and
Fire Emblem on the cable as wired, Emerald and Golden Sun both on Wireless);
a CX with a CX II in both orders; and a forced nonce tie (gpsp-host
--link-nonce-tie: both hellos tie twice, roll again, link)."""
import importlib.util, os
from linklib import Scenario, wait, press

HERE = os.path.dirname(os.path.abspath(__file__))
# nspire_link.c's LINK_PROTOCOL, read from it so a new protocol needs no edit here.
LINK_PROTOCOL = int(next(line.split()[2] for line in open(os.path.join(HERE, "..", "..", "..", "nspire_link.c"))
                         if line.startswith("#define LINK_PROTOCOL ")))


def load(name):
    spec = importlib.util.spec_from_file_location("link_scenarios_%s_for_compat" % name,
                                                  os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


emerald = load("emerald")
advance_wars = load("advance_wars")

GOLDEN_SUN = "Golden Sun (USA, Europe).gba"
FIRE_EMBLEM = "Fire Emblem - The Sacred Stones (USA, Australia).gba"

QUICK, EXACT, ADAPTER = "Link cable, quick", "Link cable, exact", "Wireless Adapter"
NO_ADAPTER = "the other GBA has no Wireless Adapter"
OLD_EXACT = "the other gpSP is older: exact only"


def linked_as(*expected):
    """The status each player linked with (link_linked; e24876d's reports only
    its last status, which is checked to have been linked), and the protocol:
    1 with e24876d, else this build's newest (LINK_PROTOCOL, 3 since the
    measured delay). expected: each player's (device, why or None)."""
    def check(run):
        got, ok = [], True
        legacy = any(p.legacy for p in run.players)
        for p, (device, why) in zip(run.players, expected):
            if p.legacy:
                got.append("e24876d: " + p.report.get("link", "-"))
                ok = ok and p.linked()
                continue
            want = "Linked: player %d (%s)%s" % (p.number, device, ": " + why if why else "")
            status = p.report.get("link_linked", "-")
            got.append(status)
            ok = ok and status == want and p.report.get("link_protocol") == ("1" if legacy else str(LINK_PROTOCOL))
        return ok, " / ".join(got)
    return check


def both(*checks):
    def check(run):
        results = [c(run) for c in checks]
        return all(r[0] for r in results), "; ".join(r[1] for r in results)
    return check


def emerald_trade(name, desk, devices, expected, models=("cx2", "cx2"), hosts=("new", "new"), late=False,
                  delays=(0,)):
    scripts = emerald.wireless_scripts(emerald.trade) if desk == "direct" else \
        emerald.cable_scripts(emerald.trade)
    if late:
        scripts = late_link(scripts)
    return Scenario(name, emerald.ROM, scripts,
                    both(emerald.traded, linked_as(*expected), hellos(2 if "legacy" in hosts else 1)), models=models,
                    devices=devices, prepare=emerald.at_desk(desk), needs=["gba_bios.bin", emerald.SAVE],
                    hosts=hosts, delays=delays)


def late_link(scripts):
    """The same, but linking only once both stand in the Pokemon Center, the
    save loaded (BOOT's end): until then each game runs with its setting's
    device (Auto: Emerald's adapter, since power-on)."""
    def moved():
        p1, p2 = scripts()
        out = []
        for lines in (p1, p2):
            at = lines.index("link")
            boot_end = len(emerald.BOOT) - 1   # BOOT's last line: wait(1)
            out.append(lines[:at] + lines[at + 1:boot_end] + ["link"] + lines[boot_end:])
        return tuple(out)
    return moved


def aw_battle(name, devices, expected, hosts=("new", "new")):
    return Scenario(name, advance_wars.ROM, advance_wars.battle_scripts(("cx2", "cx2")),
                    both(advance_wars.same_battle, linked_as(*expected), hellos(2 if "legacy" in hosts else 1)),
                    devices=devices,
                    needs=["gba_bios.bin"], hosts=hosts, delays=(0,))


def idle():
    """Linked at 1 s, then 20 s of each game on its own (title screens,
    intros): linked through, no hang, no crash."""
    lines = wait(1) + ["link"] + wait(20) + press("del") + wait(1)
    return lines, lines


def idle_menu():
    """idle, then player 1 opens the menu on the Link line (the card: the
    status, why) and closes it; player 2 runs on meanwhile."""
    p1, p2 = idle()
    look = ["press menu", "wait 0.5"] + ["press down", "wait 0.15"] * 3 + ["shot why", "press esc", "wait 1"]
    return p1 + look, p2 + wait(4)


def stayed_linked(run):
    ok = all(p.number_of("link_frames") >= 20 * 59 for p in run.players)
    return ok, "linked frames %s / %s" % tuple(p.report.get("link_frames") for p in run.players)


def hello():
    return wait(1) + ["link"] + wait(2), wait(1) + ["link"] + wait(2)


def hellos(n):
    """Each player of this build sent n hellos (link_hellos_sent): a tie
    twice makes 3; with e24876d, its MSG_HELLO_EXT and the answer, 2."""
    def check(run):
        got = [p.report.get("link_hellos_sent", "-") for p in run.players if not p.legacy]
        return all(g == str(n) for g in got), "hellos sent %s" % "/".join(got)
    return check


def scenarios():
    out = []
    # With e24876d, both orders.
    for order, hosts in (("new_old", ("new", "legacy")), ("old_new", ("legacy", "new"))):
        out.append(emerald_trade("compat_%s_cable" % order, "cable", ("exact", "exact"),
                                 ((EXACT, None), (EXACT, None)), hosts=hosts))
        out.append(emerald_trade("compat_%s_wireless" % order, "direct", ("auto", "auto"),
                                 ((ADAPTER, None), (ADAPTER, None)), hosts=hosts))
        out.append(aw_battle("compat_%s_aw" % order, ("auto", "auto"),
                             ((EXACT, OLD_EXACT), (EXACT, OLD_EXACT)), hosts=hosts))
    # Two of this build, settings that differ.
    out += [
        emerald_trade("mixed_auto_cable", "cable", ("auto", "cable"), ((QUICK, NO_ADAPTER), (QUICK, None)),
                      delays=(0, 3)),
        emerald_trade("mixed_auto_cable_late", "cable", ("auto", "cable"), ((QUICK, NO_ADAPTER), (QUICK, None)),
                      late=True),
        emerald_trade("mixed_auto_exact", "cable", ("auto", "exact"), ((EXACT, NO_ADAPTER), (EXACT, None))),
        emerald_trade("mixed_wireless_cable", "cable", ("wireless", "cable"),
                      ((QUICK, NO_ADAPTER), (QUICK, None))),
        emerald_trade("mixed_exact_wireless", "cable", ("exact", "wireless"), ((EXACT, None), (EXACT, NO_ADAPTER))),
        emerald_trade("mixed_cable_auto_cx2_cx", "cable", ("cable", "auto"), ((QUICK, None), (QUICK, NO_ADAPTER)),
                      models=("cx2", "cx")),
        emerald_trade("mixed_cable_auto_cx_cx2", "cable", ("cable", "auto"), ((QUICK, None), (QUICK, NO_ADAPTER)),
                      models=("cx", "cx2")),
        aw_battle("mixed_aw_auto_exact", ("auto", "exact"), ((EXACT, "the other GBA is set to exact"), (EXACT, None))),
    ]
    # Two different games.
    for name, roms, devices, expected in (
            ("games_emerald_gs", (emerald.ROM, GOLDEN_SUN), ("auto", "auto"),
             ((EXACT, NO_ADAPTER), (EXACT, "the other game's link is different"))),
            ("games_aw_fe", (advance_wars.ROM, FIRE_EMBLEM), ("auto", "auto"),
             ((EXACT, "the other game's link is different"), (EXACT, "the other game's link is different"))),
            ("games_wireless", (emerald.ROM, GOLDEN_SUN), ("wireless", "wireless"), ((ADAPTER, None), (ADAPTER, None)))):
        out.append(Scenario(name, roms, idle_menu if name == "games_emerald_gs" else idle,
                            both(stayed_linked, linked_as(*expected)), devices=devices,
                            needs=["gba_bios.bin"], delays=(0,), quick=name == "games_emerald_gs"))
    # A nonce tie, twice: both roll again by themselves.
    out.append(Scenario("compat_nonce_tie", emerald.ROM, hello,
                        both(linked_as((ADAPTER, None), (ADAPTER, None)), hellos(3)),
                        args=(("--link-nonce-tie", "2"),) * 2, delays=(0,), quick=True))
    return out
