#!/usr/bin/env python3
"""Browse the virtual USB folder and start a real game on the host frontend."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_tests as h

image = os.path.abspath(sys.argv[1])
KIND = os.path.splitext(os.path.basename(image))[0]   # fat16, fat32 (each one's run folders its own: both run side by side)
r = h.Run("usb-direct-" + KIND, roms=())
r.run("wait 1\nshot folders\npress enter\nwait 1\nshot usb_root\n"
      "press enter\nwait 1\nshot usb_games\npress down\npress enter\nwait 3\n"
      "press enter\nwait 2\nshot playing\nquit\n", env={"GPSP_USB_IMAGE": image})
h.check(r.notes["folders"]["ui"] == "list:0", "no games: gpSP opens on the grid with the drive's folder as its first item: %s" % r.notes["folders"]["ui"])
h.check(r.notes["usb_root"]["ui"] == "usb:0", "virtual USB folder opens")
h.check(r.notes["usb_games"]["ui"] == "usb:0", "drive folder opens")
h.check(r.notes["playing"]["frame"] > 30 and
        r.notes["playing"]["game"] == "Long name with spaces.gba", "USB ROM starts")

r2 = h.Run("usb-hotkey-" + KIND, roms=("Local.gba.tns",))
r2.run("wait 1\nshot games_list\npress u\nwait 1\nshot usb_root\n"
       "press enter\nwait 1\npress down\npress enter\nwait 3\n"
       "press enter\nwait 2\nshot playing\nquit\n", env={"GPSP_USB_IMAGE": image})
h.check(r2.notes["games_list"]["ui"] == "list:0", "local Games list opens first")
h.check(r2.notes["usb_root"]["ui"] == "usb:0", "u hotkey opens USB drive directly from Games")
h.check(r2.notes["playing"]["frame"] > 30 and
        r2.notes["playing"]["game"] == "Long name with spaces.gba", "USB ROM starts from Games u hotkey")
# The drive pulled out and plugged back in (the same hub port or another:
# the port's registers don't move): in the drive's page it opens again by
# itself; pulled and put back while another game plays, a USB game picked
# on the paused menu's Games (U: the drive's page, not through the drive's page, which was
# the only place a drive was opened: it read "Couldn't read this game. Is
# it a GBA ROM?") loads; and the drive's page opens in the folder it was
# left in while the same drive stays opened.
MAIN_GAMES = h.to_main("game_list") + "press enter\nwait 1\n"   # (the paused row's Games, wherever it is)
r3 = h.Run("usb-replug-" + KIND, roms=("Local.gba.tns",))
r3.run("wait 1\npress u\nwait 1\nshot usb_root\npad image out\nwait 1\nshot usb_gone\n"
       "pad image in\nwait 2\nshot usb_back\npress enter\nwait 1\npress down\npress enter\nwait 3\n"
       "press enter\nwait 2\nshot playing_usb\n"
       "press menu\nwait 0.5\n" + MAIN_GAMES + "press enter\nwait 3\nshot playing_local\n"
       "pad image out\nwait 1\nshot playing_unplugged\npad image in\nwait 1\n"
       "press menu\nwait 0.5\n" + MAIN_GAMES + "press u\nwait 1\npress enter\nwait 1\npress down\npress enter\nwait 3\npress enter\nwait 2\nshot playing_again\n"
       "quit\n",
       env={"GPSP_USB_IMAGE": image})
n = r3.notes
h.check(n["usb_root"]["ui"] == "usb:0" and n["usb_gone"]["ui"] == "usb:0" and n["usb_back"]["ui"] == "usb:0",
        "replug: the drive's page stays up through the drive leaving and coming back")
h.check(n["playing_usb"]["game"] == "Long name with spaces.gba" and n["playing_usb"]["frame"] > 30,
        "replug: the drive put back in its page lists and plays: %s" % n["playing_usb"])
h.check(n["playing_local"]["game"].startswith("Local") and n["playing_unplugged"]["frame"] > n["playing_local"]["frame"],
        "replug: the calculator's game plays on with the drive out: %s" % n["playing_local"])
h.check(n["playing_again"]["game"] == "Long name with spaces.gba" and n["playing_again"]["ui"] == "",
        "replug: the USB game picked on the paused menu after the drive came back loads: %s" % n["playing_again"])
said = [line for line in open(os.path.join(r3.out, "pad.txt")) if "osd " in line]
h.check(not any("GBA ROM" in line or "Couldn't read this game" in line for line in said),
        "replug: no \"Couldn't read this game\": %s" % said)

# The drive's page opens in the folder it was left in, while the same drive
# stays opened: from the games' folder one enter is a game, not a folder.
r4 = h.Run("usb-folder-" + KIND, roms=("Local.gba.tns",))
r4.run("wait 1\npress u\nwait 1\npress enter\nwait 1\npress down\npress enter\nwait 3\npress enter\nwait 2\n"
       "shot first\npress menu\nwait 0.5\n" + MAIN_GAMES + "press u\nwait 1.5\nshot usb_again\n"
       "press down\npress down\npress enter\nwait 3\npress enter\nwait 2\nshot second\nquit\n",
       env={"GPSP_USB_IMAGE": image})
n = r4.notes
h.check(n["first"]["game"] == "Long name with spaces.gba", "folder: a game from the drive's games folder: %s" % n["first"])
h.check(n["usb_again"]["ui"] == "usb:0" and n["second"]["ui"] == "" and n["second"]["frame"] > 30 and
        n["second"]["game"] not in ("-", "Long name with spaces.gba"),
        "folder: the drive's page opens where it was left: the folder's next game is two lines down: %s" % n["second"])
# save17: the saves' kept memory (~1 MB, nspire_states.c) never starves a drive's ROM, which must fit in
# memory whole (the ROM buffer grows to it): it is given back for the load and taken again after only with 2 MB
# left. One absolute heap for every build: what r16 holds with the calculator's game running (a 2 MB ROM buffer)
# and 6.5 MB more, for the drive's 8 MB game (6 MB more of ROM buffer).
KEPT = 425984 + 454839 + 144576
cfg = h.owner_config()
cfg[31] = 2                      # a 2 MB ROM buffer
probe = h.Run("usb-rom-memory-probe-" + KIND, roms=("Local.gba.tns",), files={"gpsp_qol.cfg.tns": cfg})
probe.run("wait 1\npress enter\nwait 2\nnote local\nquit\n", env={"GPSP_USB_IMAGE": image})
limit = (int(probe.notes["local"]["heap"]) - KEPT) // 1024 + 6656
r5 = h.Run("usb-rom-memory-" + KIND, roms=("Local.gba.tns",), files={"gpsp_qol.cfg.tns": cfg})
r5.run("wait 1\npress enter\nwait 2\nnote local\npress menu\nwait 0.5\n" + MAIN_GAMES + "press u\nwait 1\n"
       "press enter\nwait 1\npress down\npress enter\nwait 3\npress enter\nwait 2\nshot playing\nquit\n",
       env={"GPSP_USB_IMAGE": image}, options=["--heap-limit", str(limit)])
n = r5.notes
log = open(r5.save("gpsp_link.txt.tns"), errors="replace").read() if os.path.exists(r5.save("gpsp_link.txt.tns")) else ""
h.check(n["playing"]["game"] == "Long name with spaces.gba" and
        n["playing"]["frame"] > 30, "memory: the drive's 8 MB game loads with the saves' memory given back for it: %s" %
        n["playing"])
h.check("states: memory for saves and loads NOT KEPT" in log,
        "memory: after it, too little left to keep the saves' memory: said in the link log: %s" %
        [l for l in log.splitlines() if "states:" in l])

# save18: a local cache shrunk for saves can still grow to the USB ROM's required size, then shrink again
# when returning to a local game. USB ROM bytes must stay resident even when that leaves no save workspace.
cfg = h.owner_config()
cfg[31] = 8
seed = h.Run("usb-save-room-seed-" + KIND, roms=("Local",), files={"gpsp_qol.cfg.tns": cfg})
seed.run("wait 1\npress enter\nwait 2\npoke 2000100:5A\npress S\nwait 2\nnote local\n"
         "press Q\nwait 1\nquit\n", env={"GPSP_USB_IMAGE": image})
limit = (int(seed.notes["local"]["heap"]) - KEPT) // 1024 + 500
r6 = h.Run("usb-save-room-switch-" + KIND, reuse=seed)
r6.run("wait 1\npress enter\nwait 2\nnote local\npress menu\nwait 0.5\n" + MAIN_GAMES +
       "press u\nwait 1\npress enter\nwait 1\npress down\npress enter\nwait 3\npress enter\nwait 2\n"
       "note usb\nwait 1\nnote usb_running\npress menu\nwait 0.5\n" + MAIN_GAMES +
       "press enter\nwait 3\nnote back\npeek restored 2000100:1\npress S\nwait 2\npress Q\nwait 1\nquit\n",
       env={"GPSP_USB_IMAGE": image}, options=["--heap-limit", str(limit)])
n = r6.notes
h.check(int(n["local"]["rom_mb"]) < 8, "save18 USB: local cache shrunk for saves: %s" % n["local"])
h.check(n["usb"]["game"] == "Long name with spaces.gba" and int(n["usb"]["rom_mb"]) == 8 and
        n["usb_running"]["frame"] > n["usb"]["frame"],
        "save18 USB: full 8 MB ROM stays resident and plays: %s" % n["usb"])
h.check(n["back"]["game"] == "Local.gba.tns" and int(n["back"]["rom_mb"]) < 8,
        "save18 USB: local cache shrinks again: %s" % n["back"])
h.check(open(os.path.join(r6.out, "restored.bin"), "rb").read() == b"\x5a",
        "save18 USB: returning restores saved RAM")
h.check("Saved to slot 1" in h.osd_toasts(r6) and not [t for t in h.osd_toasts(r6) if t.startswith("Couldn't")],
        "save18 USB: saving works on return: %s" % h.osd_toasts(r6))
print(f"drive UI: {h.checks} checks, {len(h.failures)} failed")
sys.exit(bool(h.failures))
