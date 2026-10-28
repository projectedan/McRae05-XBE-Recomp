# tracing_refs.py fonts_dir out_dir [scale]: references for redrawing the game's font sheets by hand.
# For each sheet (*.dds with its *.pcf), at scale (4 by default):
#   <font>_reference.png  the letters dark on white (the game stores them white), pixels enlarged as they are
#   <font>_boxes.png      transparent, each letter's box outlined: a redrawn letter stays inside its own box
#   <font>_letters.csv    each letter's character, code and box, in sheet pixels and scaled
# and README.txt with the name each finished sheet takes in textures\replacement\ (any size, PNG with alpha).
import csv
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dds  # noqa: E402
import pcf  # noqa: E402
import texkey  # noqa: E402


def write_png(path, w, h, rgba):
    rows = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, p):
        return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def sheet_alpha(path):
    r = dds.read(path)
    rows = r[2] if isinstance(r, tuple) and len(r) == 3 else r
    return [[px[3] for px in row] for row in rows]


def main():
    src, out = sys.argv[1], sys.argv[2]
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    os.makedirs(out, exist_ok=True)
    names = []
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith('.pcf'):
            continue
        font = f[:-4]
        sheet = os.path.join(src, font + '.dds')
        if not os.path.exists(sheet):
            continue
        alpha = sheet_alpha(sheet)
        h, w = len(alpha), len(alpha[0])
        W, H = w * k, h * k
        # the letters dark on white, each sheet pixel a k x k block
        ref = bytearray(W * H * 4)
        for y in range(H):
            row = alpha[y // k]
            for x in range(W):
                v = 255 - row[x // k]
                ref[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes((v, v, v, 255))
        write_png(os.path.join(out, font + '_reference.png'), W, H, ref)
        # each letter's box, a scaled pixel wide, red
        boxes = bytearray(W * H * 4)
        table = pcf.read(os.path.join(src, f))
        glyphs = [g for g in table['glyphs'] if g['x'] + g['w'] <= w and g['y'] + g['h'] <= h and g['w'] and g['h']]

        def dot(x, y):
            if 0 <= x < W and 0 <= y < H:
                boxes[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes((230, 30, 30, 255))
        for g in glyphs:
            x0, y0, x1, y1 = g['x'] * k, g['y'] * k, (g['x'] + g['w']) * k - 1, (g['y'] + g['h']) * k - 1
            for x in range(x0, x1 + 1):
                dot(x, y0)
                dot(x, y1)
            for y in range(y0, y1 + 1):
                dot(x0, y)
                dot(x1, y)
        write_png(os.path.join(out, font + '_boxes.png'), W, H, boxes)
        with open(os.path.join(out, font + '_letters.csv'), 'w', newline='', encoding='utf-8') as c:
            wr = csv.writer(c)
            wr.writerow(['char', 'code', 'x', 'y', 'w', 'h', 'x_scaled', 'y_scaled', 'w_scaled', 'h_scaled'])
            for g in glyphs:
                ch = bytes([g['code']]).decode('cp1252', errors='replace')
                wr.writerow([ch, g['code'], g['x'], g['y'], g['w'], g['h'], g['x'] * k, g['y'] * k, g['w'] * k,
                             g['h'] * k])
        names.append((font, w, h, texkey.dds_name(sheet), len(glyphs)))
        print('%-10s %dx%d, %d letters' % (font, w, h, len(glyphs)))
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as r:
        r.write('Font sheets of Colin McRae Rally 2005, enlarged %dx, for redrawing by hand.\n\n' % k)
        r.write('<font>_reference.png  the letters dark on white (the game stores them white on transparent)\n')
        r.write('<font>_boxes.png      each letter\'s box: a redrawn letter must stay inside its own box\n')
        r.write('<font>_letters.csv    each letter\'s box in sheet pixels and at this scale\n\n')
        r.write('A finished sheet: white letters on a transparent background, PNG with alpha, the same layout at any\n')
        r.write('multiple of the sheet\'s size, saved into textures\\replacement\\ beside CMR2005.exe as:\n\n')
        for font, w, h, name, n in names:
            r.write('  %-10s %4dx%-4d  %s\n' % (font, w, h, name))
        r.write('\nThese are made from the game\'s own files: keep them on this PC, and draw the new letters\n')
        r.write('yourself (or from a freely licensed typeface) rather than tracing the originals\' shapes.\n')
    print('README.txt')


if __name__ == '__main__':
    main()
