# Draws the app icon (packaging/icons/bzzrt.svg) in the overlay style: a pixel "B" from src/common/font8x8.h
# over the histogram colours. Laid out on a 32-unit grid in even steps so it stays sharp at 16 px.
# Usage: python3 tools/make_icon.py src/common/font8x8.h packaging/icons/bzzrt.svg
import re, sys
font_src, out = sys.argv[1], sys.argv[2]
rows = re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){8})\}', open(font_src).read())
FONT = [[int(v, 16) for v in r.replace(',', ' ').split()] for r in rows]
WHITE, YELLOW, RED, GREEN = '#ebebeb', '#ffdc3c', '#ff5046', '#5ae66e'
o = ['<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32" shape-rendering="crispEdges">',
     '<rect width="32" height="32" rx="7" fill="#333333"/>',              # the app's gray screen as a thin rim
     '<rect x="1" y="1" width="30" height="30" rx="6" fill="#0a0a0a"/>']  # the overlay card
glyph, cell, gx0, gy0 = FONT[ord('B') - 32], 2, 9, 6
d = []
for gy, bits in enumerate(glyph):
    for gx in range(8):
        if bits >> gx & 1:
            d.append(f'M{gx0 + gx * cell} {gy0 + gy * cell}h{cell}v{cell}h-{cell}z')
o.append(f'<path fill="{WHITE}" d="{"".join(d)}"/>')
# frame-interval colours: above the VRR maximum, inside the range, LFC zone
for x0, x1, c in ((6, 10, YELLOW), (10, 18, GREEN), (18, 26, RED)):
    o.append(f'<rect x="{x0}" y="24" width="{x1 - x0}" height="2" fill="{c}"/>')
o.append('</svg>')
open(out, 'w').write('\n'.join(o) + '\n')
