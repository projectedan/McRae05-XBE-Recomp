# sdf_demo.py out.png sheet.bmp... (EDGE=0.5 env: the coverage the letter edge sits at): a capture's font sheet (tex_*.bmp)
# sdf_demo.py out.png sheet.bmp... : for each font sheet, its first text rows drawn at 3x as now (bilinear) and from a
# distance field built from it (8x upsampled coverage, exact distance transform, 4x field), white over dark grey.
import sys, struct, zlib, math
SCALE, UP, FIELD = 3, 8, 4
import os
EDGE = float(os.environ.get("EDGE", "0.5"))
INF = 1e20
def load_alpha(p):
    d = open(p, 'rb').read(); off, = struct.unpack_from('<I', d, 10); w, h = struct.unpack_from('<ii', d, 18)
    td = h < 0; h = abs(h)
    return w, h, [[d[off + ((y if td else h - 1 - y) * w + x) * 4 + 3] / 255.0 for x in range(w)] for y in range(h)]
def rows_of(a, w, h):
    used = [max(r) > 0.1 for r in a]; out = []; y = 0
    while y < h:
        if used[y]:
            y0 = y
            while y < h and used[y]: y += 1
            out.append((max(0, y0 - 2), min(h, y + 2)))
        y += 1
    return out
def bil(img, w, h, x, y):
    x -= 0.5; y -= 0.5; x0 = math.floor(x); y0 = math.floor(y); fx = x - x0; fy = y - y0
    def g(xx, yy): return img[min(max(yy, 0), h - 1)][min(max(xx, 0), w - 1)]
    return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy
def edt1(f):
    n = len(f); d = [0.0] * n; v = [0] * n; z = [0.0] * (n + 1); k = 0; v[0] = 0; z[0] = -INF; z[1] = INF
    for q in range(1, n):
        while True:
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k])
            if s <= z[k]: k -= 1
            else: break
        k += 1; v[k] = q; z[k] = s; z[k + 1] = INF
    k = 0
    for q in range(n):
        while z[k + 1] < q: k += 1
        d[q] = (q - v[k]) ** 2 + f[v[k]]
    return d
def edt(mask, w, h):
    g = [[0.0 if mask[y][x] else INF for x in range(w)] for y in range(h)]
    for x in range(w):
        col = edt1([g[y][x] for y in range(h)])
        for y in range(h): g[y][x] = col[y]
    return [[math.sqrt(v) for v in edt1(g[y])] for y in range(h)]
def field(a, w, h):
    W, H = w * UP, h * UP
    hi = [[bil(a, w, h, (x + 0.5) / UP, (y + 0.5) / UP) >= EDGE for x in range(W)] for y in range(H)]
    din = edt(hi, W, H); dout = edt([[not v for v in r] for r in hi], W, H)
    # signed distance in source texels, positive inside, sampled at FIELD x the source resolution
    st = UP // FIELD
    return [[(dout[y][x] - din[y][x]) / UP for x in range(st // 2, W, st)] for y in range(st // 2, H, st)]
def render(a, w, h, f):
    out_now, out_sdf = [], []
    fw, fh = w * FIELD, h * FIELD
    for y in range(h * SCALE):
        rn, rs = [], []
        for x in range(w * SCALE):
            u, v = (x + 0.5) / SCALE, (y + 0.5) / SCALE
            rn.append(min(1.0, max(0.0, bil(a, w, h, u, v))))
            d = bil(f, fw, fh, u * FIELD, v * FIELD) * SCALE  # in screen pixels
            rs.append(min(1.0, max(0.0, 0.5 + d)))
        out_now.append(rn); out_sdf.append(rs)
    return out_now, out_sdf
def png(path, img):
    h = len(img); w = len(img[0])
    raw = b''.join(b'\0' + bytes(int(40 + 215 * c) for c in r for _ in range(3)) for r in img)
    def chunk(t, p): return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
panels = []
for p in sys.argv[2:]:
    w, h, a = load_alpha(p)
    for y0, y1 in rows_of(a, w, h)[1:3]:
        crop = [r[:] for r in a[y0:y1]]
        f = field(crop, w, y1 - y0)
        now, sdf = render(crop, w, y1 - y0, f)
        panels += [now, [[0.0] * len(now[0])] * 6, sdf, [[0.0] * len(now[0])] * 24]
        print('row', p.split('/')[-1], y0, y1, file=sys.stderr)
png(sys.argv[1], [r for pnl in panels for r in pnl])
