"""The link tests' parts (tests/link/run.py runs them).

A scenario is two GBAs, each a gpsp-host (tests/host/host_hw.c) with its
calculator's model, files and script, linked by a socket (host_link.c) whose
messages take a given time to come; it passes when its check says so. The
scenarios are in tests/link/scenarios/*.py: each file's scenarios() gives a
list of Scenario.

Scripts count the game's frames (gpsp-host's "frames" lines), not time, so a
scenario plays the same at any link speed: wait(s) and press(key) give the
lines for s seconds and a key press at the GBA's 59.73 frames a second."""
import os, shutil, struct, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# GPSP_TEST_BUILD=build-san: the sanitizer build (tests/san.sh), its runs in work-san.
BUILD = os.environ.get("GPSP_TEST_BUILD", "build")
HOST = os.path.join(HERE, "..", "host", BUILD, "gpsp-host")
# e24876d's gpsp-host (the CX CAS's link build, protocol 1), built in its
# own worktree: Scenario(hosts=...) "legacy" plays a player with it.
LEGACY_HOST = os.environ.get("GPSP_LEGACY_HOST", "")
WORK = os.path.join(HERE, "work" if BUILD == "build" else "work-" + BUILD.replace("build-", ""))
GAMES = os.environ.get("GBA_GAMES", "")
BIOS = os.path.join(GAMES, "gba_bios.bin")

FPS = 16777216 / 280896   # the GBA's frames a second


def frames(n):
    return ["frames %d" % n]


def wait(seconds):
    """seconds of the game's time, in its frames."""
    return frames(round(seconds * FPS))


def press(key, down=0.1, after=0.15):
    """A key down 0.1 s and up 0.15 s, as gpsp-host's press, in frames."""
    return ["hold " + key] + wait(down) + ["release " + key] + wait(after)


def presses(key, n, gap):
    """n presses, gap seconds after each."""
    return sum((press(key) + wait(gap) for _ in range(n)), [])


def owner_settings():
    """The reference CX II settings (tests/host/host_tests.py owner_config):
    scaled/blended 3:2, frameskip off, video.cc, Partial SMC, the FPS
    counter, gpSP's keys; as gpsp.cfg.tns's bytes."""
    keys = [0x67, 0x57, 0x28, 0x24, 0x5a, 0x52, 0x5b, 0x64, 0x79, 0x7a, 0x66, 0x41, 0x31, 0x17, 0x22]
    o = [1, 1, 0, 0, 1, 0, 4] + keys + [0, 1, 2, 1, 0xFF000000, 0, 1, 1, 1, 16, 0, 1, 0, 0, 0, 0x334C4F51]
    o += keys + [0, 0x54, 0x56, 0, 0]
    o += [0, 1, 1, 0, 1, 9, 0x63, 0x53, 1] + [0] * 5
    return struct.pack("<%dI" % len(o), *o)


# The calculators: costs (gpsp-host's cost lines) and settings (None:
# gpSP's own, as installed).
#
# cx2: the reference CX II with their settings. Emerald's emulation ~4 ms a
# frame and ~25 us a line drawn (the calculator's speed tests); frameskip
# off, so every frame is drawn and the link's time shows as game speed.
#
# cx: an original CX, three times slower (132-150 MHz against 396), with
# gpSP's settings as installed: automatic frameskip, which it needs (24 ms
# a drawn frame).
#
# Each message the link sends or receives costs the calculator 0.1 ms on a
# CX II (three times on a CX): NavNet's work for a packet (the call into
# the OS, the packet and its checksum, the USB driver, the other side's
# acknowledgement, a task switch), guessed at ~40,000 cycles of the 396 MHz
# ARM926. Not measured yet: the time a message takes to come is the
# link's delay, which the matrix tries from 0 to 10 ms.
MODELS = {
    "cx2": (["cost frame 4", "cost line 25", "cost send 0.1", "cost receive 0.1"], owner_settings()),
    "cx": (["cost frame 12", "cost line 75", "cost send 0.3", "cost receive 0.3"], None),
}


class Scenario:
    """name: what run.py calls it. rom: the game's file in $GBA_GAMES.
    models: player 1's and player 2's calculators (MODELS). devices: the
    two's Link setting (gpsp-host --link-device: auto, wireless, cable,
    exact). prepare(player, saves): puts player 1's or 2's files (its
    in-game save...) into its zgpSP_files folder, where gpSP also finds
    gba_bios.bin.tns. scripts(): the two's lines (wait, press, frames...;
    the model's costs go before and quit after). check(run): from the
    finished Run, (passed, what to print). needs: files in $GBA_GAMES (or
    full paths) without which it's skipped. args: more gpsp-host options
    for each. delays: the link delays (ms) run.py --matrix runs it at.
    quick: run.py runs it when no names are given. rom can be two, one a
    player. hosts: each player's gpsp-host, "new" (this build's) or
    "legacy" (e24876d's, LEGACY_HOST: its socket's messages are bare, its
    scripts have no frames, measure, peek or message costs, which run()
    turns into waits or leaves out, and its Link cable is exact)."""

    def __init__(self, name, rom, scripts, check, models=("cx2", "cx2"), devices=("auto", "auto"),
                 prepare=None, needs=(), args=((), ()), delays=(), quick=False, hosts=("new", "new")):
        self.name, self.rom, self.scripts, self.check = name, rom, scripts, check
        self.models, self.devices, self.prepare, self.args = models, devices, prepare, args
        self.roms = (rom, rom) if isinstance(rom, str) else tuple(rom)
        self.needs = list(dict.fromkeys(self.roms)) + list(needs)
        self.delays, self.quick, self.hosts = delays, quick, hosts

    def missing(self):
        out = [n for n in self.needs if not os.path.exists(os.path.join(GAMES, n))]
        if "legacy" in self.hosts and not os.path.exists(LEGACY_HOST):
            out.append(LEGACY_HOST)
        return out


def saves_folder(legacy):
    """Where a gpsp-host keeps its BIOS, settings and saves: zgpSP_files
    (since 2c4633d's layout), or e24876d's zgpSP_saves, which it reads its
    older gpsp.cfg.tns from too. With the new folder e24876d found no save
    and no BIOS, and the compat runs with it failed."""
    return "zgpSP_saves" if legacy else "zgpSP_files"


class Player:
    """One finished gpsp-host: its folders, report lines (report["link_fps"]
    is "59.73"), the notes and shots by name, and what it said on stderr."""

    def __init__(self, work, number, exit_code, legacy=False):
        self.number, self.exit, self.legacy = number, exit_code, legacy
        self.dir = os.path.join(work, "p%d" % number)
        self.stderr = open(os.path.join(self.dir, "stderr.txt")).read()
        self.out = os.path.join(self.dir, "out")
        self.saves = os.path.join(self.dir, "docs", saves_folder(legacy))
        self.report, self.notes = {}, {}
        path = os.path.join(self.out, "report.txt")
        for line in open(path) if os.path.exists(path) else []:
            parts = line.split()
            if parts and parts[0] in ("shot", "note"):
                self.notes[parts[1]] = line
            elif parts:
                self.report[parts[0]] = " ".join(parts[1:])

    def number_of(self, key):
        try:
            return float(self.report.get(key, "nan"))
        except ValueError:
            return float("nan")

    def linked(self):
        """Whether the game ran linked (frames counted while linked; e24876d's
        reports only its status at the end: linked, or the other gone)."""
        if self.legacy:
            return self.report.get("link", "").startswith(("Linked", "The link cable came out",
                                                            "The other GBA unlinked"))
        return self.number_of("link_frames") > 0


class Run:
    def __init__(self, scenario, delay, work, players):
        self.scenario, self.delay, self.work, self.players = scenario, delay, work, players


def legacy_script(lines):
    """A script for e24876d's gpsp-host: frames as the time they take at full
    speed (its virtual time is only the frames', waiting on the link
    doesn't count), no measure, peek or message costs."""
    out = []
    for line in lines:
        word = line.split()
        if word and word[0] == "frames":
            out.append("wait %.6f" % (int(word[1]) / FPS))
        elif word and (word[0] in ("measure", "peek") or word[:2] in (["cost", "send"], ["cost", "receive"])):
            continue
        else:
            out.append(line)
    return out


def run(scenario, delay=0):
    """Plays the scenario once, both players at once, the link's messages
    taking delay ms (one way). Its folder: tests/link/work/NAME@DELAY; each
    player's in p1 and p2 (docs: its My Documents, out: report.txt and the
    shots). The socket is a short name there (a socket's path can't be
    long)."""
    work = os.path.join(WORK, "%s@%g" % (scenario.name, delay))
    shutil.rmtree(work, ignore_errors=True)
    scripts = scenario.scripts()
    procs = []
    for p in (1, 2):
        model = scenario.models[p - 1]
        costs, settings = MODELS[model]
        legacy = scenario.hosts[p - 1] == "legacy"
        rom = scenario.roms[p - 1]
        docs = os.path.join(work, "p%d" % p, "docs")
        gba, saves = os.path.join(docs, "gba"), os.path.join(docs, saves_folder(legacy))
        os.makedirs(gba)
        os.makedirs(saves)
        os.makedirs(os.path.join(work, "p%d" % p, "out"))
        os.symlink(os.path.join(GAMES, rom), os.path.join(gba, rom))
        if os.path.exists(BIOS):
            shutil.copy(BIOS, os.path.join(saves, "gba_bios.bin.tns"))
        if settings and legacy:
            # e24876d's gpsp-host sets --link-device before the settings
            # file loads, which then has the last word: word 68 is it.
            words = list(struct.unpack("<%dI" % (len(settings) // 4), settings))
            words[68] = {"wireless": 1, "cable": 2, "exact": 2}.get(scenario.devices[p - 1], 0)
            settings = struct.pack("<%dI" % len(words), *words)
        if settings:
            with open(os.path.join(saves, "gpsp.cfg.tns"), "wb") as f:
                f.write(settings)
        if scenario.prepare:
            scenario.prepare(p, saves)
        script = os.path.join(work, "p%d.txt" % p)
        lines = costs + scripts[p - 1] + ["quit"]
        with open(script, "w") as f:
            f.write("\n".join(legacy_script(lines) if legacy else lines) + "\n")
        device = scenario.devices[p - 1]
        if legacy:
            cmd = ["timeout", "300", LEGACY_HOST, "--model", model, "--link", "sock", "--player", str(p),
                   "--link-device", "cable" if device == "exact" else device]
        else:
            cmd = ["timeout", "300", HOST, "--model", model, "--link", "sock",
                   "--player", str(p), "--link-device", device, "--link-delay", str(delay)]
            if "legacy" in scenario.hosts:
                cmd += ["--link-peer", "legacy"]
        cmd += list(scenario.args[p - 1])
        cmd += [script, os.path.join(work, "p%d" % p, "out"), gba, rom]
        with open(os.path.join(work, "p%d" % p, "stderr.txt"), "w") as err:
            procs.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err, cwd=work))
    players = [Player(work, p, proc.wait(), scenario.hosts[p - 1] == "legacy") for p, proc in enumerate(procs, 1)]
    return Run(scenario, delay, work, players)
