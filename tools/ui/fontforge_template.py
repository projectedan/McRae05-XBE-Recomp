# fontforge_template.py assets/fonts out_dir: a FontForge template (.sfd) for each of CMR 2005's fonts, to draw a font of
# one's own that drops into the game's layout. Each template holds every character the game's font has, its advance
# set to the width of the character's box on the game's sheet, that box drawn in the glyph's background layer, and
# guides for the baseline, cap height, x-height, ascender and descender measured from the game's sheet. Units: the box
# height is 1000, so one sheet pixel is 1000 / box height units. No outlines and no pixels of the game: only sizes.
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dds  # noqa: E402
import pcf  # noqa: E402

ON = 128  # alpha counted as ink


def ink_rows(px, g):
    rows = [y for y in range(g['y'], min(g['y'] + g['h'], len(px)))
            if any(px[y][x][3] >= ON for x in range(g['x'], min(g['x'] + g['w'], len(px[0]))))]
    return (rows[0] - g['y'], rows[-1] - g['y'] + 1) if rows else None


def measure(px, glyphs):
    by = {chr(g['code']): g for g in glyphs if 32 < g['code'] < 128}
    def tops_bottoms(chars):
        r = [ink_rows(px, by[c]) for c in chars if c in by]
        return [t for t, b in r if r and (t, b)] if False else [x for x in r if x]
    flat = tops_bottoms('HEFILTNMKZ0123456789axzvwunmrs')
    baseline = statistics.median(b for t, b in flat) if flat else None
    def top(chars):
        r = tops_bottoms(chars)
        return statistics.median(t for t, b in r) if r else None
    def bottom(chars):
        r = tops_bottoms(chars)
        return statistics.median(b for t, b in r) if r else None
    return {'baseline': baseline, 'cap': top('HEFILTNMKZ'), 'x': top('xzvwunmrs'), 'ascender': top('bdhkl'),
            'descender': bottom('pqgyj')}


def char_of(code):
    try:
        ch = bytes([code]).decode('cp1252')
    except UnicodeDecodeError:
        return 0xE000 + code  # no Windows-1252 character: likely a button icon, in the private use area
    return ord(ch)


def write_sfd(path, name, font, m):
    glyphs = font['glyphs']
    h = statistics.mode(g['h'] for g in glyphs)
    s = 1000.0 / h
    base = m['baseline'] if m['baseline'] is not None else h * 0.8
    ascent = round(base * s)
    descent = 1000 - ascent
    u = lambda px_from_top: round((base - px_from_top) * s, 1)  # sheet rows from the box top to units above baseline
    lines = ['SplineFontDB: 3.0', 'FontName: CMR_' + name, 'FullName: CMR ' + name + ' (template)',
             'FamilyName: CMR ' + name, 'Weight: Regular', 'Copyright: ', 'Version: 001.000', 'ItalicAngle: 0',
             'UnderlinePosition: -100', 'UnderlineWidth: 50', 'Ascent: %d' % ascent, 'Descent: %d' % descent,
             'LayerCount: 2', 'Layer: 0 0 "Back" 1', 'Layer: 1 0 "Fore" 0', 'Encoding: UnicodeBmp',
             'DisplaySize: -48', 'AntiAlias: 1', 'FitToEm: 0',
             '# CMR 2005 %s: sheet box height %d px, 1 px = %.2f units, line height %d px' % (name, h, s, font['line'])]
    # guides across every glyph window: baseline, then the measured heights
    guides = [('baseline', 0.0)]
    for key, label in (('cap', 'cap height'), ('x', 'x-height'), ('ascender', 'ascender'), ('descender', 'descender')):
        if m[key] is not None:
            guides.append((label, u(m[key])))
    lines.append('Grid')
    for label, y in guides:
        lines += ['-1000 %s m 0' % y, ' 4000 %s l 1024' % y]
    lines.append('EndSplineSet')
    lines.append('BeginChars: 65536 %d' % len(glyphs))
    for gid, g in enumerate(glyphs):
        uni = char_of(g['code'])
        w = round(g['w'] * s)
        top, bottom = round(base * s), round((base - g['h']) * s)
        lines += ['', 'StartChar: uni%04X' % uni, 'Encoding: %d %d %d' % (uni, uni, gid), 'Width: %d' % w,
                  'Flags: W', 'LayerCount: 2', 'Back', 'SplineSet',
                  '0 %d m 1' % bottom, ' %d %d l 1' % (w, bottom), ' %d %d l 1' % (w, top), ' 0 %d l 1' % top,
                  ' 0 %d l 1' % bottom, 'EndSplineSet', 'EndChar']
    lines += ['EndChars', 'EndSplineFont', '']
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    return h, s, guides


def main(src, out):
    os.makedirs(out, exist_ok=True)
    rows = []
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith('.pcf'):
            continue
        name = f[:-4]
        font = pcf.read(os.path.join(src, f))
        w, hgt, px = dds.read(os.path.join(src, name + '.dds'))
        m = measure(px, font['glyphs'])
        h, s, guides = write_sfd(os.path.join(out, name + '.sfd'), name, font, m)
        chars = ''.join(chr(char_of(g['code'])) if char_of(g['code']) < 0xE000 else '?' for g in font['glyphs'])
        rows.append((name, len(font['glyphs']), h, s, guides, chars))
        print('%-10s %3d glyphs, box %2d px, %5.1f units/px, guides %s' % (
            name, len(font['glyphs']), h, s, ', '.join('%s %g' % g for g in guides)))
    with open(os.path.join(out, 'fonts.txt'), 'w', encoding='utf-8') as t:
        for name, n, h, s, guides, chars in rows:
            t.write('%s: %d glyphs, box %d px tall (1 px = %.2f units)\n  guides: %s\n  characters: %s\n\n' % (
                name, n, h, s, ', '.join('%s %g' % g for g in guides), chars))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
