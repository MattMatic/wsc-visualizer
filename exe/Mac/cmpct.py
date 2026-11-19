#!/usr/bin/env python3
"""cmpct.py - compare a Core Text dump (ShapingCompareMac) with HarfBuzz.

  python3 cmpct.py Gulzara.ct.txt font.ttf [--tol 2] [--script arab] [--lang urd] [--rtl] [-o Gulzara.ct.wsc.txt] [--features "liga=0"]

Both engines list glyphs in visual (left to right) order, so no RTL folding: drawn x = pen + x_offset for both.
Words differ if the glyph ids differ or (--width) the total width differs (> tol), or a non-blank glyph's drawn x / y differs (> tol).
Writes a .wsc-style file (hb: / ct# lines, differing words only) and prints a summary.
Needs: pip install uharfbuzz fonttools"""
import sys, argparse, collections, time
import uharfbuzz as hb
print('cmpct.py 1.3 (bidi W2/W7, Latin runs as latn, visual order, edge width, progress)', file=sys.stderr)
from fontTools.ttLib import TTFont

ap = argparse.ArgumentParser()
ap.add_argument('ct'); ap.add_argument('font')
ap.add_argument('--tol', type=float, default=2); ap.add_argument('--script', default='arab'); ap.add_argument('--lang', default='urd')
ap.add_argument('--rtl', action='store_true', help='RTL direction for HarfBuzz (use when the dump was made with -R)')
ap.add_argument('--features', default='')
ap.add_argument('-o', '--out')
ap.add_argument('--width', action='store_true', help='also compare total width (right edge of the rightmost base glyph); off by default because engines fold offsets into advances differently')
ap.add_argument('--blanks', action='store_true', help='also compare offsets of blank glyphs')
a = ap.parse_args()

face = hb.Face(hb.Blob.from_file_path(a.font)); font = hb.Font(face); font.scale = (1000, 1000)
order = TTFont(a.font).getGlyphOrder()
feats = {}
for item in [x.strip() for x in a.features.split(',') if x.strip()]:
    v = 1
    if item[0] == '+': item = item[1:]
    elif item[0] == '-': item = item[1:]; v = 0
    if '=' in item: item, vs = item.split('='); v = int(vs)
    feats[item] = v
blank = {}
def is_blank(g):
    if g not in blank:
        e = font.get_glyph_extents(g)
        blank[g] = (e is None) or (e.width == 0 and e.height == 0)
    return blank[g]

import unicodedata
def bidi_levels(w):
    # Small UAX#9 subset for an RTL paragraph (what Core Text applies and HarfBuzz does not): W1 NSM/BN, W2/W3, W4/W5, W7, separators and terminators
    # next to European numbers, N1/N2 neutrals, I1 levels. Enough for Arabic letters, digits, Latin and punctuation.
    t = [unicodedata.bidirectional(c) for c in w]
    for i, c in enumerate(t):                       # W1 (BN is treated like NSM: it joins the previous run)
        if c in ('NSM', 'BN'): t[i] = t[i-1] if i else 'R'
    def prev_strong(i, kinds):
        for k in range(i-1, -1, -1):
            if t[k] in kinds: return t[k]
        return 'R'                                  # sos of an RTL paragraph
    for i, c in enumerate(t):                       # W2: EN after AL (nearest strong) becomes AN
        if c == 'EN' and prev_strong(i, ('R', 'L', 'AL')) == 'AL': t[i] = 'AN'
    t = ['R' if c == 'AL' else c for c in t]        # W3
    for i, c in enumerate(t):                       # W4
        if c in ('ES', 'CS') and 0 < i < len(t)-1 and t[i-1] == t[i+1] and t[i-1] in ('EN', 'AN'): t[i] = t[i-1]
    for i in range(len(t)):                         # W5
        if t[i] == 'ET':
            j = i
            while j < len(t) and t[j] == 'ET': j += 1
            k = i
            while k >= 0 and t[k] == 'ET': k -= 1
            if (j < len(t) and t[j] == 'EN') or (k >= 0 and t[k] == 'EN'):
                for m in range(k+1, j): t[m] = 'EN'
    t = ['ON' if c in ('ES', 'ET', 'CS') else c for c in t]   # W6
    for i, c in enumerate(t):                       # W7: EN after L (nearest strong, sos = R) becomes L  (cmpct 1.3: 'COVID-19', 'BBC 5,650')
        if c == 'EN' and prev_strong(i, ('R', 'L')) == 'L': t[i] = 'L'
    strong = lambda c: 'L' if c == 'L' else ('R' if c in ('R', 'EN', 'AN') else None)   # N0/N1: EN and AN count as R
    lv = [None] * len(t)
    for i, c in enumerate(t):
        if strong(c): lv[i] = c
    for i, c in enumerate(t):                       # N1/N2
        if lv[i] is None:
            p = next((strong(t[k]) for k in range(i-1, -1, -1) if strong(t[k])), 'R')
            n = next((strong(t[k]) for k in range(i+1, len(t)) if strong(t[k])), 'R')
            lv[i] = p if p == n else 'R'
    out = []
    for i, c in enumerate(t):                       # I1/I2 on an RTL paragraph (base level 1)
        if c in ('EN', 'AN'): out.append(2)
        elif lv[i] == 'L': out.append(2)
        else: out.append(1)
    return out

def reorder(levels):
    idx = list(range(len(levels)))
    for L in range(max(levels), 0, -1):
        i = 0
        while i < len(idx):
            if levels[idx[i]] >= L:
                j = i
                while j < len(idx) and levels[idx[j]] >= L: j += 1
                idx[i:j] = idx[i:j][::-1]; i = j
            else: i += 1
    return idx

def run_script(txt):
    # Core Text itemizes by script: a run with Latin letters is shaped as Latin (e.g. hyphen.latin, not hyphen.urdu); cmpct 1.3
    if any(unicodedata.name(c, '').startswith('LATIN') for c in txt): return 'latn', 'en'
    return a.script, a.lang

def shape_run(txt, rtl):
    sc, lg = run_script(txt)
    b = hb.Buffer(); b.add_str(txt); b.script = sc; b.language = lg
    b.direction = 'rtl' if rtl else 'ltr'
    hb.shape(font, b, feats)
    return [(i.codepoint, p.x_offset, p.y_offset, p.x_advance) for i, p in zip(b.glyph_infos, b.glyph_positions)]

def hb_shape(w):
    if not a.rtl: return shape_run(w, False)
    lv = bidi_levels(w)
    runs = []                                       # logical runs of equal level
    for i, c in enumerate(w):
        if runs and runs[-1][0] == lv[i]: runs[-1][1] += c
        else: runs.append([lv[i], c])
    if len(runs) == 1: return shape_run(w, runs[0][0] % 2 == 1)
    r = []
    for k in reorder([x[0] for x in runs]):         # visual (left to right) order of runs; each run is already visual
        r += shape_run(runs[k][1], runs[k][0] % 2 == 1)
    return r

def parse_run(body):
    r = []
    for it in body.split(';'):
        if it: r.append(tuple(float(x) for x in it.split(',')[:4]))   # dump format 1.2 adds an optional 5th field (rtl run flag)
    return r

def drawn(r):
    # drawn x = pen + offset. Width = right edge of the rightmost base glyph, max(x + advance) over glyphs with advance > 0: Core Text gives attached marks a NEGATIVE advance (pen restored), HarfBuzz a zero advance
    # plus an offset, so the plain sum of advances differs although every glyph is drawn in the same place.
    x = 0; o = []; edge = 0
    for g, dx, dy, adv in r:
        o.append((g, x + dx, dy))
        if adv > 0: edge = max(edge, x + dx + adv)
        x += adv
    return o, edge

words = []; cur = None
for raw in open(a.ct, encoding='utf-8'):
    line = raw.rstrip('\r\n')
    if not line or line.startswith('\t#'): continue
    if line.startswith('\tct:'): words.append((cur, parse_run(line[4:])))
    elif not line.startswith('\t'): cur = line
res = []; kinds = collections.Counter(); ex = collections.defaultdict(list); gl = collections.Counter()
t0 = time.time()
for idx, (w, ct) in enumerate(words):
    if (idx + 1) % 2000 == 0 or idx + 1 == len(words):
        el = time.time() - t0
        sys.stderr.write('\r%d / %d words  (%.0f s, about %.0f s left)   ' % (idx + 1, len(words), el, el / (idx + 1) * (len(words) - idx - 1)))
        sys.stderr.flush()
    h = hb_shape(w)
    if [int(x[0]) for x in ct] != [x[0] for x in h]:
        k = 'glyph'
    else:
        dc, wc = drawn(ct); dh, wh = drawn(h)
        k = 'ok'
        if a.width and abs(wc - wh) > a.tol: k = 'width'
        for (g, xc, yc), (_, xh, yh) in zip(dc, dh):
            if is_blank(int(g)) and not a.blanks: continue
            if abs(xc - xh) > a.tol or abs(yc - yh) > a.tol:
                k = 'position' if k == 'ok' else k; gl[order[int(g)]] += 1
    kinds[k] += 1
    if k != 'ok':
        ex[k].append(w); res.append((w, h, ct))
sys.stderr.write('\n')
n = len(words)
print('words %d, differing %d (%.1f%% match)' % (n, n - kinds['ok'], 100.0 * kinds['ok'] / max(n, 1)))
for k in ('glyph', 'width', 'position'):
    print('  %-9s %6d   e.g. %s' % (k, kinds[k], ' | '.join(ex[k][:4])))
print('glyphs most often misplaced:', gl.most_common(10))
if a.out:
    f = lambda r: ''.join('%d,%d,%d,%d;' % (g, dx, dy, adv) for g, dx, dy, adv in r)
    with open(a.out, 'w', encoding='utf-8') as o:
        nm = TTFont(a.font)['name']      # the wsc-visualizer matches the loaded font on these two names
        o.write('\t# FONTFILE:\t%s\n\t# FONTNAME:\t%s\n\t# FONTVERS:\t%s\n\t# TOLERANCE:\t%g\n\t# TOOL:\tcmpct.py 1.3 (Core Text vs HarfBuzz)\n\t# RTL:\t%s\n\t# COMPARE:\tdrawn-positions(visual order)\n\t# ENGINES:\thb ct(coretext)\n'
                % (a.font, nm.getDebugName(4), nm.getDebugName(5), a.tol, 'yes' if a.rtl else 'no'))
        for w, h, ct in res: o.write('%s\n\thb:%s\n\tct#%s\n' % (w, f(h), f(ct)))
    print('wrote', a.out)
