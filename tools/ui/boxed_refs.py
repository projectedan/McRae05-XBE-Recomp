# boxed_refs.py fonts_dir ref_dir [scale]: alignment copies of the references tracing_refs.py and dump_2d_only made.
#   <ref_dir>/fonts/<font>_reference_boxes.png      the enlarged sheet with each letter's box drawn in
#   <ref_dir>/ui_textures_boxes/<name>.png         each dumped UI texture enlarged, its edge outlined, and boxed:
#       a font sheet (its name is a font's texture name) by its letters' boxes, a texture with transparency around
#       each separate shape (pixels not fully transparent); an opaque one only at its edge (compression noise in
#       its background makes shapes there meaningless)
# Boxes are red, a scaled pixel wide, drawn just outside what they enclose; the texture's edge is blue.
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcf  # noqa: E402
import texkey  # noqa: E402

RED, BLUE = (230, 30, 30, 255), (40, 110, 230, 255)


def read_png(path):
    d = open(path, 'rb').read()
    o, idat = 8, b''
    while o < len(d):
        n, t = struct.unpack_from('>I4s', d, o)
        body = d[o + 8:o + 8 + n]
        if t == b'IHDR':
            w, h, depth, ct = struct.unpack_from('>IIBB', body)
        elif t == b'IDAT':
            idat += body
        o += 12 + n
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            px = line[x * ch:(x + 1) * ch]
            if ch == 1: px = bytes((px[0], px[0], px[0], 255))
            elif ch == 2: px = bytes((px[0], px[0], px[0], px[1]))
            elif ch == 3: px = bytes(px) + b'\xff'
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = px
        prev = line
    return w, h, out


def write_png(path, w, h, rgba):
    rows = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, p):
        return struct.pack('>I', len(p)) + t + p + struct.pack('>I', zlib.crc32(t + p) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def enlarge(w, h, rgba, k):
    W, H = w * k, h * k
    out = bytearray(W * H * 4)
    for y in range(H):
        sy = y // k
        for x in range(W):
            s = (sy * w + x // k) * 4
            out[(y * W + x) * 4:(y * W + x) * 4 + 4] = rgba[s:s + 4]
    return W, H, out


def rect(W, H, img, x0, y0, x1, y1, colour):
    """the outline of [x0, x1) x [y0, y1), one pixel wide, inside those bounds"""
    for x in range(max(x0, 0), min(x1, W)):
        for y in (y0, y1 - 1):
            if 0 <= y < H: img[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes(colour)
    for y in range(max(y0, 0), min(y1, H)):
        for x in (x0, x1 - 1):
            if 0 <= x < W: img[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes(colour)


def shapes(w, h, rgba):
    """bounding boxes of the separate shapes: 4-connected runs of pixels that are not fully transparent"""
    def solid(i):
        return rgba[i * 4 + 3] > 0
    seen = bytearray(w * h)
    boxes = []
    for start in range(w * h):
        if seen[start] or not solid(start):
            continue
        seen[start] = 1
        stack = [start]
        x0, y0, x1, y1 = w, h, 0, 0
        while stack:
            i = stack.pop()
            x, y = i % w, i // w
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x + 1), max(y1, y + 1)
            for j in ((i - 1) if x else -1, (i + 1) if x + 1 < w else -1, i - w, i + w):
                if 0 <= j < w * h and not seen[j] and solid(j):
                    seen[j] = 1
                    stack.append(j)
        boxes.append((x0, y0, x1, y1))
    return boxes


def main():
    fonts_dir, ref = sys.argv[1], sys.argv[2]
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    # each font's texture name and letters
    fonts = {}
    for f in sorted(os.listdir(fonts_dir)):
        if f.lower().endswith('.pcf') and os.path.exists(os.path.join(fonts_dir, f[:-4] + '.dds')):
            sheet = os.path.join(fonts_dir, f[:-4] + '.dds')
            w, h, _ = pcf.sheet_size(sheet)
            glyphs = [g for g in pcf.read(os.path.join(fonts_dir, f))['glyphs']
                      if g['w'] and g['h'] and g['x'] + g['w'] <= w and g['y'] + g['h'] <= h]
            fonts[texkey.dds_name(sheet)] = (f[:-4], glyphs)
    # the font references with their boxes in
    fdir = os.path.join(ref, 'fonts')
    for name, (font, glyphs) in fonts.items():
        src = os.path.join(fdir, font + '_reference.png')
        if not os.path.exists(src):
            continue
        W, H, img = read_png(src)
        for g in glyphs:
            rect(W, H, img, g['x'] * k, g['y'] * k, (g['x'] + g['w']) * k, (g['y'] + g['h']) * k, RED)
        write_png(os.path.join(fdir, font + '_reference_boxes.png'), W, H, img)
        print('font', font, len(glyphs), 'boxes')
    # the dumped UI textures, enlarged with their boxes
    udir, odir = os.path.join(ref, 'ui_textures'), os.path.join(ref, 'ui_textures_boxes')
    os.makedirs(odir, exist_ok=True)
    for f in sorted(os.listdir(udir)):
        if not f.lower().endswith('.png'):
            continue
        w, h, rgba = read_png(os.path.join(udir, f))
        W, H, img = enlarge(w, h, rgba, k)
        if f in fonts:
            kind, boxes = 'letters of ' + fonts[f][0], [(g['x'], g['y'], g['x'] + g['w'], g['y'] + g['h'])
                                                       for g in fonts[f][1]]
        elif any(rgba[i] < 255 for i in range(3, len(rgba), 4)):
            kind, boxes = 'shapes', shapes(w, h, rgba)
        else:
            kind, boxes = 'opaque, edge only', []
        for x0, y0, x1, y1 in boxes:
            # just outside the shape, so its own edge pixels stay visible
            rect(W, H, img, x0 * k - 1, y0 * k - 1, x1 * k + 1, y1 * k + 1, RED)
        rect(W, H, img, 0, 0, W, H, BLUE)
        write_png(os.path.join(odir, f), W, H, img)
        print('%-40s %3d %s' % (f, len(boxes), kind))


if __name__ == '__main__':
    main()
