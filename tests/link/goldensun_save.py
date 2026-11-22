#!/usr/bin/env python3
"""A Golden Sun (USA, Europe) save for the link tests: one file, a new game's
party (Isaac alone, level 1) with a Djinn, so the title menu has Battle (the
Battle Arena, where two GBAs fight over the link cable).

    goldensun_save.py OUT.sav.tns [--name NAME]
    goldensun_save.py --record SAVE     prints file 0's link battles won and lost

--name renames the leader (5 letters at most), to tell the two GBAs apart.

The new game comes from the game itself: gpsp-host (tests/host) plays from an
empty save to Isaac's house (Start, Start, New Game, the name's END, the
first two lines) and saves a state; its EWRAM is the game's data, which the
save file holds. Done once, then kept in tests/link/work/goldensun/.

The save's layout (read from the game's save code, 0x801f818 and around):
file F is two 4 KB flash sectors, slot F and F+3, each "CAMELOT", the slot,
a 16-bit sum of its data's first 0xFE8 bytes, a counter, 4 zeros and 0xFF0
bytes of data: EWRAM 0x2000000-0x2000FF0 and 0x2001000-0x2001FF0. The data
starts with the file's summary, which the title's menus read."""
import argparse, gzip, os, shutil, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import linklib

ROM = "Golden Sun (USA, Europe).gba"
CACHE = os.path.join(linklib.WORK, "goldensun")
EWRAM = os.path.join(CACHE, "newgame.ewram")
# S is the reference save-state key (linklib.owner_settings).
NEW_GAME = ["wait 6", "press shift", "wait 2", "press shift", "wait 3", "press del", "wait 2",
            "press del", "wait 2", "press del", "wait 3", "press del", "wait 3", "press S", "wait 1", "quit"]


def state_ewram(path):
    """EWRAM out of a gpSP state file: a 32-byte head (magic, version,
    counter, sizes, flags: 2 = not packed) and zlib'd BSON whose ewram is a
    binary field."""
    raw = open(path, "rb").read()
    packed, flags = struct.unpack_from("<II", raw, 16)
    body = raw[32:32 + packed]
    data = body if flags & 2 else zlib.decompress(body)
    i = data.index(b"\x05ewram\x00") + 7
    n = struct.unpack_from("<I", data, i)[0]
    return data[i + 5:i + 5 + n]


def new_game():
    """The new game's EWRAM (made once)."""
    if os.path.exists(EWRAM):
        return open(EWRAM, "rb").read()
    work = os.path.join(CACHE, "newgame.%d" % os.getpid())
    shutil.rmtree(work, ignore_errors=True)
    gba, saves, out = (os.path.join(work, d) for d in ("docs/gba", "docs/zgpSP_files", "out"))
    for d in (gba, saves, out):
        os.makedirs(d)
    os.symlink(os.path.join(linklib.GAMES, ROM), os.path.join(gba, ROM))
    shutil.copy(linklib.BIOS, os.path.join(saves, "gba_bios.bin.tns"))
    with open(os.path.join(saves, "gpsp.cfg.tns"), "wb") as f:
        f.write(linklib.owner_settings())
    script = os.path.join(work, "script.txt")
    with open(script, "w") as f:
        f.write("\n".join(NEW_GAME) + "\n")
    subprocess.run([linklib.HOST, "--model", "cx2", script, out, gba, ROM], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ew = state_ewram(os.path.join(saves, ROM[:-4] + "0.svs.tns"))
    with open(EWRAM + ".%d" % os.getpid(), "wb") as f:
        f.write(ew)
    os.replace(EWRAM + ".%d" % os.getpid(), EWRAM)
    shutil.rmtree(work, ignore_errors=True)
    return ew


def save(ew, name=None, djinn=1):
    ew = bytearray(ew)
    if name:
        ew[0x500:0x50F] = (name.encode()[:5] + bytes(15))[:15]   # the leader's name
    # The summary: the leader's name and level (level 0: an empty file), the
    # party's members, clear data (the title's Send), Djinn (its Battle).
    ew[0x00:0x0C] = ew[0x500:0x50C]
    ew[0x0C] = ew[0x50F]
    ew[0x18:0x21] = bytes([0, 0xFF, 0xFF, 0xFF, 0, 0xFF, 0, 0, 0])
    ew[0x21] = 0
    ew[0x22] = djinn
    # The ROM's build (0x8077cb8), which a saved game carries; the sum of the
    # 968 words from 0x40, which the game checks.
    ew[0x240:0x244] = struct.pack("<I", 0x1652)
    ew[0x26:0x28] = struct.pack("<H", 0x1652)
    ew[0x2C:0x30] = struct.pack("<I", sum(struct.unpack_from("<968I", ew, 0x40)) & 0xFFFFFFFF)
    flash = bytearray(b"\xff" * 0x10000)
    for sector, slot, data in ((0, 0, ew[0:0xFF0]), (1, 3, ew[0x1000:0x1FF0])):
        head = b"CAMELOT" + bytes([slot]) + struct.pack("<HH", sum(data[:0xFE8]) & 0xFFFF, 1) + bytes(4)
        flash[sector * 0x1000:(sector + 1) * 0x1000] = head + bytes(data)
    return bytes(flash)


def record(path):
    """File 0's link battles (won, lost), as the arena saves them after
    each: the newest of its sectors (slot 0, the highest counter; gpSP may
    have gzipped the save)."""
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    best = None
    for n in range(len(raw) // 0x1000):
        sector = raw[n * 0x1000:(n + 1) * 0x1000]
        counter = struct.unpack_from("<H", sector, 10)[0]
        if sector[:8] == b"CAMELOT\0" and (best is None or counter > best[0]):
            best = (counter, sector[16:])
    return struct.unpack_from("<HH", best[1], 0x4EC) if best else (0, 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("out", nargs="?")
    p.add_argument("--name")
    p.add_argument("--record")
    a = p.parse_args()
    if a.record:
        print("won %d lost %d" % record(a.record))
    elif a.out:
        with open(a.out, "wb") as f:
            f.write(save(new_game(), a.name))
    else:
        p.error("no OUT")


if __name__ == "__main__":
    main()
