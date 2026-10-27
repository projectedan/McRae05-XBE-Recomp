# pcf.py: CMR 2005's font tables (assets/fonts/*.pcf), each beside its sheet (*.dds).
# Layout: six int32 (3, glyph count, line height, then three more: the third is the count of whatever follows the
# records), a u16 glyph index per character code 0..255 (0xFFFF none), then a 16 byte record per glyph: u16 x, y,
# width, height on the sheet, u8 character code (Windows-1252), u8 count of its kerning pairs, 2 bytes unused, u32
# the pairs' place in the kerning table that follows the records.
#   python pcf.py assets/fonts/*.pcf     lists each font
import os
import struct
import sys


def read(path):
    d = open(path, 'rb').read()
    ver, n, line, a, b, c = struct.unpack_from('<6i', d, 0)
    start = 0x18 + 256 * 2
    recs = [struct.unpack_from('<4H2B', d, start + i * 16) for i in range(n)]
    glyphs = [{'code': r[4], 'x': r[0], 'y': r[1], 'w': r[2], 'h': r[3], 'kerns': r[5]} for r in recs]
    return {'line': line, 'extra': (a, b, c), 'glyphs': glyphs, 'trailing': len(d) - (start + n * 16)}


def sheet_size(dds_path):
    d = open(dds_path, 'rb').read(128)
    h, w = struct.unpack_from('<II', d, 12)
    return w, h, d[84:88].decode('ascii', 'replace')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        f = read(p)
        g = f['glyphs']
        w, h, fmt = sheet_size(p[:-4] + '.dds')
        print('%-12s %3d glyphs, line %2d, extra %-12s trailing %4d bytes, sheet %dx%d %s, glyphs to %dx%d, codes %d..%d' % (
            os.path.basename(p), len(g), f['line'], str(f['extra']), f['trailing'], w, h, fmt,
            max(x['x'] + x['w'] for x in g), max(x['y'] + x['h'] for x in g), min(x['code'] for x in g),
            max(x['code'] for x in g)))
