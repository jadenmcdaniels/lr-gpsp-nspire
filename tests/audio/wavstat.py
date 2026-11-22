"""Peak and RMS of a 16 bit WAV file, per side."""
import array
import math
import sys
import wave

w = wave.open(sys.argv[1], 'rb')
ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
a = array.array('h')
a.frombytes(w.readframes(n))
if sys.byteorder == 'big':
    a.byteswap()
print('%s: %d Hz, %d channel(s), %.2f s' % (sys.argv[1], rate, ch, n / float(rate)))
for c in range(ch):
    x = a[c::ch]
    if not len(x):
        continue
    peak = max(max(x), -min(x))
    rms = math.sqrt(sum(v * v for v in x) / len(x))
    clipped = sum(1 for v in x if v >= 32767 or v <= -32768)
    zero = sum(1 for v in x if v == 0)
    print('  %s: peak %5d (%.1f dBFS)  rms %7.1f (%.1f dBFS)  clipped %d  zero samples %.1f%%'
          % ('LR'[c] if ch == 2 else 'ch%d' % c, peak, 20 * math.log10(max(peak, 1) / 32768.0), rms,
             20 * math.log10(max(rms, 1e-9) / 32768.0), clipped, 100.0 * zero / len(x)))
