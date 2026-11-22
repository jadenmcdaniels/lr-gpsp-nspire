#!/usr/bin/env python3
"""Sound is reachable, settings save, cancel works, and returning resumes (r19 third pass: Volume is Settings' first tile,
0-20/20, a range between < >; Sound delay left the menus: the settings file keeps it)."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_tests as h
import r19nav as N

more = N.settings_index("more")
r = h.Run("sound-settings")
r.run("wait 1\npress enter\nwait 1\npress menu\nwait 0.3\n" + N.settings_tile("volume") +
      "wait 0.3\nshot sound\n"
      # Volume: 14/20 -> 10/20 (enter, four left, enter)
      + h.change("left", 4) +
      # Volume again: put between the chevrons, moved, and put back with esc
      "press enter\npress right\npress right\npress right\npress esc\nshot cancelled\n"
      "press esc\nwait 0.3\nshot row\npress esc\nwait 1\nshot playing\npress Q\nwait 1\nquit\n")
h.check(r.notes["sound"]["ui"] == "Settings:0", "Volume is Settings' first tile: %s" % r.notes["sound"]["ui"])
h.check(r.notes["cancelled"]["ui"] == "Settings:0", "cancelled volume edit stays on its tile: %s" % r.notes["cancelled"]["ui"])
h.check(r.notes["row"]["ui"] == "main:%d" % h.MAIN["settings"], "esc on the tiles goes back to the top row: %s" % r.notes["row"]["ui"])
h.check(r.notes["playing"]["frame"] > r.notes["sound"]["frame"], "leaving Settings resumes the game")
cfg = h.read_words(r.save("gpsp.cfg.tns"))
h.check(cfg[3] == 0xA52A0051, "volume 10/20 persists (word 3 bit 21: 20 steps); the cancelled 13/20 was "
        "discarded: %08x" % cfg[3])
print(f"sound UI: {h.checks} checks, {len(h.failures)} failed")
sys.exit(bool(h.failures))
