"""The cost table of tests/audio/speed.sh: reads the model's files (SCENE.noaudio.*,
SCENE.audio.*) in a directory and prints one line per scene."""
import os
import sys

MHZ = {'cx2': 396.0, 'cx192': 192.0, 'cx': 132.0}
SCALE = 1.05      # as calc_speed.py


def model(path, skip):
    """Per frame cycles by part, over the frames after skip."""
    lines = open(path).read().split('\n')
    names = lines[2].split()[1:]
    n = len(names)
    frames = []
    for line in lines:
        if line.startswith('frame'):
            v = [int(x) for x in line.split()[1:1 + n]]
            frames.append(dict(zip(names, v)))
    timed = frames[skip:]
    return {k: sum(f[k] for f in timed) / float(len(timed)) for k in names}, len(timed)


def main():
    calc, top, d, scenes = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4:]
    skips = {'aw': 30, 'emerald': 30, 'fe': 1700, 'gs': 1700, 'mc': 1700}
    ms = lambda c: c * SCALE / (MHZ[calc] * 1000.0)
    print('%-8s %9s %9s %8s | %8s %11s %8s | calc %s, ms per frame (model x 1.05)'
          % ('scene', 'game', 'game', 'audio', 'audio', 'audio', 'share', calc))
    print('%-8s %9s %9s %8s | %8s %11s %8s |' % ('', 'no audio', 'with', 'adds', 'part ms', 'part cycles', 'of frame'))
    for s in scenes:
        skip = skips[s]
        a, _ = model(os.path.join(d, s + '.noaudio.model'), skip)
        b, nf = model(os.path.join(d, s + '.audio.model'), skip)
        game = ('cpu', 'translate', 'events', 'dma', 'audio')
        ga = sum(a[k] for k in game)
        gb = sum(b[k] for k in game)
        total = sum(v for k, v in b.items())
        print('%-8s %9.2f %9.2f %8.2f | %8.3f %11.0f %7.1f%% |  (%d frames timed, frame %.2f ms)'
              % (s, ms(ga), ms(gb), ms(gb - ga), ms(b['audio']), b['audio'], 100.0 * b['audio'] / total, nf,
                 ms(total)))
    for s in scenes if top else []:
        txt = open(os.path.join(d, s + '.audio.txt')).read().split('\n')
        rows = [l for l in txt if l.startswith('audio ')]
        if rows:
            print('\n%s: the audio part by function (per frame over all frames run)' % s)
            print('part     function                                          insns   cycles   CPI  ifetch    data    wbuf     ms')
            for r in rows[:top]:
                print(r)


main()
