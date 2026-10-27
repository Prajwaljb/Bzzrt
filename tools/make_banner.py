# Draws docs/banner.svg (and the 1280x640 social preview) in the app's overlay style from src/common/font8x8.h.
# Usage: python3 tools/make_banner.py src/common/font8x8.h docs/banner.svg 1280 400
#        python3 tools/make_banner.py src/common/font8x8.h social.svg 1280 640
import math, re, sys
font_src, out, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rows = re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}', open(font_src).read())
FONT = [[int(v, 16) for v in r.replace(',', ' ').split()] for r in rows]
assert len(FONT) == 96
WHITE, GRAY, YELLOW, RED, GREEN = '#ebebeb', '#a0a0a0', '#ffdc3c', '#ff5046', '#5ae66e'
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" shape-rendering="crispEdges">']
def rect(x, y, w, h, c, a=1):
    o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{c}"' + (f' fill-opacity="{a}"' if a < 1 else '') + '/>')
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
rect(0, 0, W, H, '#333333')  # the app's dark gray test pattern
# overlay card
ch = 320; cy = (H - ch) // 2
rect(40, cy, W - 80, ch, '#000', .84)
text(84, cy + 28, 'SYNCHORA', WHITE, 6)
sub = 'VRR & LFC ANALYSIS'
text(W - 84 - 24 * len(sub), cy + 40, sub, YELLOW, 3)
# present interval histogram, 0-50 ms in 0.5 ms bins, VRR range 48-200 Hz
bins, bw, hh = 101, 11, 140
hx, hy = 84, cy + 120
counts = [0.0] * bins
def bump(ms, n, spread):
    for i in range(bins):
        counts[i] += n * math.exp(-(((i + .5) * .5 - ms) / spread) ** 2)
bump(6.9, 4000, .55); bump(16.7, 180, .5); bump(22.5, 60, .7); bump(31, 8, .6); bump(4.4, 12, .3)
counts[-1] = 3
rect(hx, hy, bins * bw, hh, '#1e1e1e')
lo, hi = 1000 / 200, 1000 / 48
lx, zx = hx + round(lo / .5 * bw), hx + round(hi / .5 * bw)
rect(zx, hy, hx + bins * bw - zx, hh, RED, .10)  # LFC zone: intervals longer than the VRR minimum allows
lmax = math.log10(1 + max(counts))
for i, n in enumerate(counts):
    if n < .5: continue
    c = (i + .5) * .5
    col = RED if (i == bins - 1 or c > hi) else (YELLOW if c < lo else GREEN)
    h = max(2, round(hh * math.log10(1 + n) / lmax))
    rect(hx + i * bw, hy + hh - h, bw - 1, h, col)
rect(lx, hy, 2, hh, YELLOW); rect(zx, hy, 2, hh, RED)
text(lx - 40, hy - 24, '200 Hz', YELLOW, 2)
text(zx - 32, hy - 24, '48 Hz', RED, 2)
text(zx + 14, hy + 12, 'LFC', RED, 3)
ly = hy + hh + 16
text(hx, ly, '0', GRAY, 2); text(hx + 50 * bw - 16, ly, '25 ms', GRAY, 2); text(hx + 96 * bw - 8, ly, '50+', GRAY, 2)
if H >= 500:
    url = 'github.com/Prajwaljb/Synchora'
    text((W - 16 * len(url)) // 2, cy + ch + 48, url, GRAY, 2)
o.append('</svg>')
open(out, 'w').write('\n'.join(o) + '\n')
