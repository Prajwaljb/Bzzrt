# Draws docs/banner.svg (and the 1280x640 social preview) in the app's overlay style from src/common/font8x8.h.
# Usage: python3 tools/make_banner.py src/common/font8x8.h docs/banner.svg 1280 400
#        python3 tools/make_banner.py src/common/font8x8.h social.svg 1280 640
import math, re, sys
font_src, out, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rows = re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}', open(font_src).read())
FONT = [[int(v, 16) for v in r.replace(',', ' ').split()] for r in rows]
assert len(FONT) == 96
WHITE, GRAY, CYAN, YELLOW, RED, GREEN = '#ebebeb', '#a0a0a0', '#5ad2ff', '#ffdc3c', '#ff5046', '#5ae66e'
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">',
     f'<rect width="{W}" height="{H}" fill="#333333"/>']
def text(x, y, s, color, sc):
    parts = []
    for i, ch in enumerate(s):
        g = FONT[ord(ch) - 32]
        for gy in range(8):
            gx = 0
            while gx < 8:
                if g[gy] >> gx & 1:
                    run = gx
                    while run < 8 and g[gy] >> run & 1: run += 1
                    parts.append(f'M{x + (i*8 + gx)*sc} {y + gy*sc}h{(run-gx)*sc}v{sc}h{-(run-gx)*sc}z')
                    gx = run
                else: gx += 1
    o.append(f'<path fill="{color}" d="{"".join(parts)}"/>')
    return 8 * sc * len(s)
# overlay card
cx, cw = 40, W - 80
ch = 312; cy = (H - ch) // 2
o.append(f'<rect x="{cx}" y="{cy}" width="{cw}" height="{ch}" fill="#000" fill-opacity=".84"/>')
px = cx + 44; py = cy + 76
text(px, py, 'SYNCHORA', WHITE, 9)
text(px, py + 100, 'VRR FLICKER ANALYSIS', YELLOW, 3)
text(px, py + 146, 'Windows  Linux  macOS', GRAY, 2)
# present interval histogram, 0-50 ms in 0.5 ms bins, VRR 48-200 Hz
bins, bw, hh = 101, 5, 150
hx = cx + cw - 44 - bins * bw - 0; hy = cy + 52
counts = [0.0] * bins
def bump(ms, n, spread):
    for i in range(bins):
        c = (i + .5) * .5
        counts[i] += n * math.exp(-((c - ms) / spread) ** 2)
bump(6.9, 4000, .55); bump(16.7, 180, .5); bump(22.5, 60, .7); bump(31, 8, .6); bump(4.4, 12, .3)
counts[bins - 1] = 3
o.append(f'<rect x="{hx}" y="{hy}" width="{bins*bw}" height="{hh}" fill="#1e1e1e"/>')
lmax = math.log10(1 + max(counts))
lo, hi = 1000 / 200, 1000 / 48
for i, n in enumerate(counts):
    if n < .5: continue
    c = (i + .5) * .5
    col = RED if (i == bins - 1 or c > hi) else (YELLOW if c < lo else GREEN)
    h = max(2, round(hh * math.log10(1 + n) / lmax))
    o.append(f'<rect x="{hx + i*bw}" y="{hy + hh - h}" width="{bw-1}" height="{h}" fill="{col}"/>')
for ms, col in ((lo, YELLOW), (hi, RED)):
    o.append(f'<rect x="{hx + round(ms / .5 * bw)}" y="{hy}" width="2" height="{hh}" fill="{col}"/>')
text(hx + round(hi / .5 * bw) + 10, hy + 10, 'LFC', RED, 2)
ly = hy + hh + 12
text(hx, ly, '0', GRAY, 2); text(hx + 50*bw - 16, ly, '25', GRAY, 2); text(hx + 96*bw - 8, ly, '50+', GRAY, 2)
text(hx, ly + 30, 'frame interval (ms)', GRAY, 2)
if H >= 500:
    url = 'github.com/Prajwaljb/Synchora'
    text((W - 16 * len(url)) // 2, cy + ch + 48, url, GRAY, 2)
o.append('</svg>')
open(out, 'w').write('\n'.join(o) + '\n')
