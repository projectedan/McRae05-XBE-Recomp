# fit_fold.py rec.wav start end: searches the stereo fold's centre, back and LFE gains (and a dialogue-like lift of
# what front left, right and centre share) for the band balance and co-driver rise of the Xbox hardware capture
# (2026-10-27, measured with bands.py). Each band of each speaker channel is filtered once; a fold is then a sum per
# 50 ms block of precomputed cross products, so many folds are cheap. Stdlib only.
import sys, array, math, itertools
d = open(sys.argv[1], 'rb').read(); i = d.find(b'data'); n = int.from_bytes(d[i+4:i+8], 'little')
pcm = array.array('h'); pcm.frombytes(d[i+8:i+8+n - n % 32])
R = 48000; a0, a1 = int(float(sys.argv[2]) * R), int(float(sys.argv[3]) * R)
ch = lambda c: [pcm[k * 16 + c] / 32767.0 for k in range(a0, a1)]
FL, FR, C, S, BL, BR = ch(0), ch(1), ch(2), ch(3), ch(4), ch(5)
# components of the mono fold L + R: fronts, centre (x2), backs, LFE (x2)
comps = [[a + b for a, b in zip(FL, FR)], [2 * v for v in C], [a + b for a, b in zip(BL, BR)], [2 * v for v in S]]
names = ['front', 'centre', 'back', 'lfe']
def filt(sig, lo, hi):
    out = sig
    for kind, f in (('hp', lo), ('lp', hi)):
        if f is None: continue
        w = 2 * math.pi * f / R; c = math.cos(w); al = math.sin(w) / (2 * 0.7071)
        if kind == 'lp': b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0
        else: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0
        A0 = 1 + al; b0 /= A0; b1 /= A0; b2 /= A0; a1 = -2 * c / A0; a2 = (1 - al) / A0
        for _ in range(2):
            y = []; x1 = x2 = y1 = y2 = 0.0
            for v in out:
                o = b0*v + b1*x1 + b2*x2 - a1*y1 - a2*y2; x2 = x1; x1 = v; y2 = y1; y1 = o; y.append(o)
            out = y
    return out
bands = [('<150', None, 150), ('150-600', 150, 600), ('600-1k', 600, 1000), ('1-2.5k', 1000, 2500), ('2.5-6k', 2500, 6000), ('>6k', 6000, None)]
xbox = {'<150': -4.4, '600-1k': -12.6, '1-2.5k': -10.5, '2.5-6k': -17.3, '>6k': -24.9}
xbox_pop = 10.2
B = R // 20
gram = {}
for name, lo, hi in bands:
    f = [filt(x, lo, hi) for x in comps]
    nb = (a1 - a0) // B
    g = []
    for b in range(nb):
        s0, s1 = b * B, (b + 1) * B
        m = [[sum(f[p][k] * f[q][k] for k in range(s0, s1)) / B for q in range(4)] for p in range(4)]
        g.append(m)
    gram[name] = g
    print('filtered', name, file=sys.stderr)
def levels(w):
    out = {}
    for name in gram:
        r = sorted(10 * math.log10(sum(w[p] * w[q] * m[p][q] for p in range(4) for q in range(4)) + 1e-12) for m in gram[name])
        out[name] = (r[len(r) // 2], r[int(0.97 * len(r))])
    return out
def score(w):
    L = levels(w); ref = L['150-600'][0]
    bal = {k: L[k][0] - ref for k in xbox}
    pop = L['1-2.5k'][1] - L['1-2.5k'][0]
    err = sum((bal[k] - xbox[k]) ** 2 for k in xbox) + 2 * (pop - xbox_pop) ** 2
    return err, bal, pop
base = [1.0, 0.707, 0.707, 0.316]
e, bal, pop = score(base)
print('now: centre 0.707 back 0.707 lfe 0.316  error %.1f  pop %.1f  ' % (e, pop) + '  '.join('%s %+.1f' % (k, bal[k]) for k in xbox))
best = None
for c, b, s in itertools.product([0.5, 0.6, 0.707, 0.85, 1.0, 1.2, 1.4], [0.5, 0.707, 1.0], [0.18, 0.22, 0.25, 0.28, 0.316, 0.35, 0.4, 0.45]):
    e, bal, pop = score([1.0, c, b, s])
    if best is None or e < best[0]: best = (e, c, b, s, bal, pop)
e, c, b, s, bal, pop = best
print('best: centre %.3f back %.3f lfe %.3f  error %.1f  pop %.1f  ' % (c, b, s, e, pop) + '  '.join('%s %+.1f' % (k, bal[k]) for k in xbox))
print('xbox:                                   pop %.1f  ' % xbox_pop + '  '.join('%s %+.1f' % (k, xbox[k]) for k in xbox))
