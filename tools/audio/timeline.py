import sys, array, math
d = open(sys.argv[1], 'rb').read(); i = d.find(b'data'); n = int.from_bytes(d[i+4:i+8], 'little')
pcm = array.array('h'); pcm.frombytes(d[i+8:i+8+n - n % 32])
C, R, B = 16, 48000, 24000  # 0.5 s blocks
cols = [(0, 'FL'), (3, 'LFE'), (2, 'C'), (6, 's13'), (9, '3D6'), (13, 'rev10'), (14, 'outL')]
nb = len(pcm) // C // B
print(' time ' + ''.join('%7s' % c for _, c in cols))
for b in range(nb):
    row = []
    for c, _ in cols:
        x = pcm[b * B * C + c: (b + 1) * B * C: C]
        s = sum(v * v for v in x) / len(x)
        row.append(10 * math.log10(s / 32767**2 + 1e-12))
    print('%5.1f ' % (b * 0.5) + ''.join('%7.1f' % v for v in row))
