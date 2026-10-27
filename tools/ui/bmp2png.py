# bmp2png.py in.bmp out.png [alpha]: 32-bit BMP to PNG; 'alpha' shows the alpha channel as grey, else colour over the
# alpha on dark grey. stdlib only.
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read(); off, = struct.unpack_from('<I', d, 10); w, h = struct.unpack_from('<ii', d, 18)
top_down = h < 0; h = abs(h); mode = sys.argv[3] if len(sys.argv) > 3 else 'rgb'
rows = []
for y in range(h):
    sy = y if top_down else h - 1 - y
    r = bytearray(b'\0')
    for x in range(w):
        b, g, rr, a = d[off + (sy * w + x) * 4: off + (sy * w + x) * 4 + 4]
        if mode == 'alpha': r += bytes((a, a, a))
        elif mode == 'over': r += bytes(int(c * a / 255 + 40 * (255 - a) / 255) for c in (rr, g, b))
        else: r += bytes((rr, g, b))
    rows.append(bytes(r))
def chunk(t, p): return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
