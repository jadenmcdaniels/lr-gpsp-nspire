#!/bin/bash
# Two linked gpsp-hosts playing Pokemon Emerald from a save made by
# emerald_desk.py, each with its own script, and a picture of each "shot":
# player 1's on top, player 2's below (WORK/sheet.png).
#
#   tests/link/pair.sh SAVE "p1 script lines;..." "p2 lines;..."
#
# Script lines are gpsp-host's (wait, tap, hold, release, shot, link, ...).
# $GBA_GAMES holds the game and gba_bios.bin (as tests/arm/run.sh). WORK
# defaults to tests/link/work; DEVICE (auto, wireless, cable or exact) is
# what's plugged into the GBAs; DELAY the ms a message takes (one way).
here=$(cd "$(dirname "$0")" && pwd)
host=$here/../host/build/gpsp-host
games=${GBA_GAMES:-}
rom="Pokemon - Emerald Version (USA, Europe)"
work=${WORK:-$here/work}
save=$1

for p in 1 2; do
  rm -rf "$work/p$p"
  mkdir -p "$work/p$p/docs/gba" "$work/p$p/docs/zgpSP_files" "$work/p$p/out"
  ln -s "$games/$rom.gba" "$work/p$p/docs/gba/"
  cp "$games/gba_bios.bin" "$work/p$p/docs/zgpSP_files/gba_bios.bin.tns"
  cp "$save" "$work/p$p/docs/zgpSP_files/$rom.sav.gz.tns"
done
echo "$2" | tr ';' '\n' > "$work/p1.txt"; echo quit >> "$work/p1.txt"
echo "$3" | tr ';' '\n' > "$work/p2.txt"; echo quit >> "$work/p2.txt"
rm -f "$work/sock"
link="--link-device ${DEVICE:-auto} --link-delay ${DELAY:-0}"
timeout 300 "$host" --link "$work/sock" --player 1 $link "$work/p1.txt" "$work/p1/out" "$work/p1/docs/gba" "$rom.gba" &
timeout 300 "$host" --link "$work/sock" --player 2 $link "$work/p2.txt" "$work/p2/out" "$work/p2/docs/gba" "$rom.gba"
wait
grep -h "^link" "$work/p1/out/report.txt" "$work/p2/out/report.txt"
python3 -I - "$work" <<'PY'
import sys
from PIL import Image
work = sys.argv[1]
names = [l.split()[1] for l in open(work + '/p1/out/report.txt') if l.startswith('shot ')]
if names:
    w, h = 160, 120
    sheet = Image.new('RGB', (w * len(names), h * 2))
    for i, n in enumerate(names):
        for p in (1, 2):
            try:
                sheet.paste(Image.open('%s/p%d/out/%s.ppm' % (work, p, n)).resize((w, h)), (i * w, (p - 1) * h))
            except OSError:
                pass
    sheet.save(work + '/sheet.png')
    print(' '.join(names))
PY
