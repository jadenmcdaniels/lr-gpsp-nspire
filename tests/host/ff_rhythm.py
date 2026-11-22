"""Fast forward's picture rhythm on the simulated calculators, the frames
costing what the speed model gave for Emerald's scenes (ff/*.txt): a line a
scene, calculator and top speed: normal play's game frames and pictures a
second, fast forward's, the gaps between new pictures on the LCD (ms:
shortest-longest, mean, spread) and in game frames (the step), and how often
the step changed from one picture to the next.
  python3 -I tests/host/ff_rhythm.py [CALCS [SCENES [TENTHS]]]
  e.g. cx2,cx192,cx town,walk,battle,wipe 0,20,30   (0: max; the defaults)
Run tests/host/run.sh (or make) first: it builds gpsp-host."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_tests as H  # noqa: E402


def main():
    calcs = sys.argv[1].split(",") if len(sys.argv) > 1 else ["cx2", "cx192", "cx"]
    scenes = sys.argv[2].split(",") if len(sys.argv) > 2 else ["town", "walk", "battle", "wipe"]
    tenths = [int(t) for t in sys.argv[3].split(",")] if len(sys.argv) > 3 else [0, 20, 30]
    os.makedirs(H.WORK, exist_ok=True)
    for calc in calcs:
        for scene in scenes:
            for t in tenths:
                print(H.ff_rhythm_line(scene, calc, t, H.ff_rhythm(scene, calc, t)), flush=True)


if __name__ == "__main__":
    main()
