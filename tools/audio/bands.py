# bands.py file.wav [left_ch right_ch start_s end_s]: block levels (50 ms) per band of the stereo mix, stdlib only
import sys, array, math
path = sys.argv[1]; d = open(path, 'rb').read()
ch = int.from_bytes(d[22:24], 'little'); rate = int.from_bytes(d[24:28], 'little')
i = d.find(b'data'); n = int.from_bytes(d[i+4:i+8], 'little'); pcm = array.array('h'); pcm.frombytes(d[i+8:i+8+n - n % (2*ch)])
lc, rc = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (0, 1)
t0, t1 = (float(sys.argv[4]), float(sys.argv[5])) if len(sys.argv) > 5 else (0, 1e9)
a0 = int(t0 * rate); a1 = min(len(pcm) // ch, int(t1 * rate))
x = [(pcm[k*ch+lc] + pcm[k*ch+rc]) / 65534.0 for k in range(a0, a1)]
def bp(sig, lo, hi):
    out = sig
    for kind, f in (('hp', lo), ('lp', hi)):
        if f is None: continue
        w = 2 * math.pi * f / rate; c = math.cos(w); al = math.sin(w) / (2 * 0.7071)
        if kind == 'lp': b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0
        else: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0
        A0 = 1 + al; b0 /= A0; b1 /= A0; b2 /= A0; a1 = -2 * c / A0; a2 = (1 - al) / A0
        for _ in range(2):
            y = []; x1 = x2 = y1 = y2 = 0.0
            for v in out:
                o = b0*v + b1*x1 + b2*x2 - a1*y1 - a2*y2; x2 = x1; x1 = v; y2 = y1; y1 = o; y.append(o)
            out = y
    return out
B = int(rate * 0.05)
def blocks(sig):
    r = []
    for k in range(0, len(sig) - B, B):
        s = sum(v*v for v in sig[k:k+B]) / B; r.append(10 * math.log10(s + 1e-12))
    return sorted(r)
def pct(r, p): return r[min(len(r) - 1, int(p / 100 * len(r)))]
print('%s  %.1f-%.1f s, %d Hz' % (path.split('/')[-1], a0 / rate, a1 / rate, rate))
print('%-14s %7s %7s %7s %7s %7s   pop p97/p99' % ('band', 'p10', 'median', 'p90', 'p97', 'p99'))
for name, lo, hi in (('all', None, None), ('<150 Hz', None, 150), ('150-600', 150, 600), ('600-1000', 600, 1000), ('1-2.5 kHz', 1000, 2500), ('2.5-6 kHz', 2500, 6000), ('>6 kHz', 6000, None)):
    r = blocks(bp(x, lo, hi) if (lo or hi) else x)
    m = pct(r, 50)
    print('%-14s %7.1f %7.1f %7.1f %7.1f %7.1f   %5.1f / %5.1f' % (name, pct(r, 10), m, pct(r, 90), pct(r, 97), pct(r, 99), pct(r, 97) - m, pct(r, 99) - m))
