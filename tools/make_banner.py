# Draws docs/banner.svg (and the 1280x640 social preview) in the app's overlay style from src/common/font8x8.h.
# Usage: python3 tools/make_banner.py src/common/font8x8.h docs/banner.svg 1280 400
#        python3 tools/make_banner.py src/common/font8x8.h social.svg 1280 640
import math, re, sys
font_src, out, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rows = re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}', open(font_src).read())
FONT = [[int(v, 16) for v in r.replace(',', ' ').split()] for r in rows]
assert len(FONT) == 96
WHITE, GRAY, YELLOW, RED, GREEN = '#ebebeb', '#a0a0a0', '#ffdc3c', '#ff5046', '#5ae66e'
social = H >= 500
o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'''<defs>
  <radialGradient id="bg" cx="0.3" cy="0.15" r="1.1">
    <stop offset="0" stop-color="#1b1d22"/><stop offset="0.55" stop-color="#101115"/><stop offset="1" stop-color="#08090b"/>
  </radialGradient>
  <pattern id="grid" width="8" height="8" patternUnits="userSpaceOnUse">
    <path d="M8 0H0V8" fill="none" stroke="#ffffff" stroke-opacity="0.035" stroke-width="1"/>
  </pattern>
  <linearGradient id="lfc" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="{RED}" stop-opacity="0.26"/><stop offset="1" stop-color="{RED}" stop-opacity="0.03"/>
  </linearGradient>
  <linearGradient id="fade" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="0.55"/>
  </linearGradient>
  <filter id="glow" x="-20%" y="-40%" width="140%" height="180%"><feGaussianBlur stdDeviation="7"/></filter>
  <clipPath id="card"><rect width="{W}" height="{H}" rx="{0 if social else 14}"/></clipPath>
</defs>''', '<g clip-path="url(#card)">',
     f'<rect width="{W}" height="{H}" fill="url(#bg)"/>', f'<rect width="{W}" height="{H}" fill="url(#grid)"/>']
def rect(x, y, w, h, c, extra=''):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{c}"{extra}/>'
def text(x, y, s, color, sc, extra=''):
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
    return f'<path fill="{color}"{extra} d="{"".join(parts)}"/>'
M = 72  # outer margin
# title row: name left, subtitle right, bottoms aligned
ty = 56 if not social else 88
o.append(text(M, ty, 'SYNCHORA', WHITE, 7, ' shape-rendering="crispEdges"'))
sub = 'VRR & LFC ANALYSIS'
o.append(text(W - M - 24 * len(sub), ty + 56 - 24, sub, YELLOW, 3, ' shape-rendering="crispEdges"'))
# frame interval histogram, 0-50 ms in 0.5 ms bins, VRR range 48-200 Hz
bins = 101
bw = (W - 2 * M) // bins
hx = M + (W - 2 * M - bins * bw) // 2
hy = ty + 56 + (60 if not social else 96)
base = H - (70 if not social else 132)
hh = base - hy
counts = [0.0] * bins
def bump(ms, n, spread):
    for i in range(bins):
        counts[i] += n * math.exp(-(((i + .5) * .5 - ms) / spread) ** 2)
bump(6.9, 4000, .55); bump(16.7, 180, .5); bump(22.5, 60, .7); bump(31, 8, .6); bump(4.4, 12, .3)
counts[-1] = 3
lo, hi = 1000 / 200, 1000 / 48
lx, zx = hx + round(lo / .5 * bw), hx + round(hi / .5 * bw)
right = hx + bins * bw
o.append(rect(zx, hy - 30, right - zx, hh + 30, 'url(#lfc)'))  # LFC zone: intervals longer than the VRR minimum allows
lmax = math.log10(1 + max(counts))
bars = []
for i, n in enumerate(counts):
    if n < .5: continue
    c = (i + .5) * .5
    col = RED if (i == bins - 1 or c > hi) else (YELLOW if c < lo else GREEN)
    h = max(3, round(hh * 0.92 * math.log10(1 + n) / lmax))
    bars.append(rect(hx + i * bw, base - h, bw - 2, h, col))
o.append('<g filter="url(#glow)" opacity="0.55">' + ''.join(bars) + '</g>')
o.append('<g shape-rendering="crispEdges">' + ''.join(bars) + '</g>')
o.append(rect(hx, base - hh, right - hx, hh, 'url(#fade)', ' opacity="0.35"'))
o.append(rect(hx, base, right - hx, 2, '#ffffff', ' fill-opacity="0.18"'))
o.append(rect(lx, hy - 30, 2, hh + 30, YELLOW, ' fill-opacity="0.85"'))
o.append(rect(zx, hy - 30, 2, hh + 30, RED))
o.append(text(lx + 12, hy - 28, '200 Hz', YELLOW, 2, ' shape-rendering="crispEdges"'))
o.append(text(zx + 12, hy - 28, '48 Hz', RED, 2, ' shape-rendering="crispEdges"'))
o.append(text(zx + 14, hy + 4, 'LFC', RED, 4, ' shape-rendering="crispEdges"'))
ly = base + 14
o.append(text(hx, ly, '0', GRAY, 2, ' fill-opacity="0.7" shape-rendering="crispEdges"'))
o.append(text(hx + 50 * bw - 8, ly, '25', GRAY, 2, ' fill-opacity="0.7" shape-rendering="crispEdges"'))
o.append(text(right - 64, ly, '50 ms', GRAY, 2, ' fill-opacity="0.7" shape-rendering="crispEdges"'))
if social:
    url = 'github.com/Prajwaljb/Synchora'
    o.append(text(M, H - 64, url, GRAY, 2, ' shape-rendering="crispEdges"'))
    p = 'Windows  Linux  macOS'
    o.append(text(W - M - 16 * len(p), H - 64, p, GRAY, 2, ' shape-rendering="crispEdges"'))
o.append('</g></svg>')
open(out, 'w').write('\n'.join(o) + '\n')
