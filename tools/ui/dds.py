# dds.py: a DXT1, DXT3 or DXT5 .dds (level 0) to RGBA rows, stdlib only. For measuring the game's font sheets.
import struct


def _565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def read(path):
    d = open(path, 'rb').read()
    h, w = struct.unpack_from('<II', d, 12)
    fourcc = d[84:88]
    block = 8 if fourcc == b'DXT1' else 16
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    o = 128
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            b = d[o:o + block]
            o += block
            alpha = [255] * 16
            if fourcc == b'DXT3':
                bits = int.from_bytes(b[:8], 'little')
                alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
            elif fourcc == b'DXT5':
                a0, a1 = b[0], b[1]
                table = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else
                                    [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                bits = int.from_bytes(b[2:8], 'little')
                alpha = [table[(bits >> (3 * i)) & 7] for i in range(16)]
            cb = b[block - 8:]
            c0, c1 = struct.unpack_from('<HH', cb, 0)
            p0, p1 = _565(c0), _565(c1)
            if c0 > c1 or block == 16:
                cols = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)), tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
            else:
                cols = [p0, p1, tuple((x + y) // 2 for x, y in zip(p0, p1)), (0, 0, 0)]
            idx = struct.unpack_from('<I', cb, 4)[0]
            for i in range(16):
                y, x = by + i // 4, bx + i % 4
                if y < h and x < w:
                    a = alpha[i] if block == 16 else (0 if (c0 <= c1 and (idx >> (2 * i)) & 3 == 3) else 255)
                    px[y][x] = cols[(idx >> (2 * i)) & 3] + (a,)
    return w, h, px
