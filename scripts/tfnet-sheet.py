#!/usr/bin/env python3
# TIE Fighter flown together (scripts/tfnet-video.py's takes): a contact sheet of two machines' recordings, host |
# guest every STEP s from FROM to TO (the host's seconds), the guest's by the wall clock (the moments for a video).
#   scripts/tfnet-sheet.py HOST_TAKE GUEST_TAKE OUT.png FROM TO STEP [cols]
import sys
import numpy as np
from PIL import Image, ImageDraw
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'walkthrough'))
from labrec import Take
h, g = Take(sys.argv[1]), Take(sys.argv[2])
t0, t1, step = map(float, sys.argv[4:7]); cols = int(sys.argv[7]) if len(sys.argv) > 7 else 3
def wm(t):
    e = [(ms, ev['ms']) for ms, ev in t.events if ev.get('t') == 'wall']
    return np.array([a for a, _ in e], float), np.array([b for _, b in e], float)
(he, hw), (ge, gw) = wm(h), wm(g)
ts = np.arange(t0, t1, step)
cw, ch = 2 * 320, 240
rows = (len(ts) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cw, rows * (ch + 14)), (0, 0, 0))
for i, t in enumerate(ts):
    hm = t * 1000; gm = np.interp(np.interp(hm, he, hw), gw, ge)
    cell = Image.new('RGB', (cw, ch))
    cell.paste(Image.fromarray(h.picture(hm)).resize((320, 240)), (0, 0))
    cell.paste(Image.fromarray(g.picture(gm)).resize((320, 240)), (320, 0))
    x, y = (i % cols) * cw, (i // cols) * (ch + 14)
    sheet.paste(cell, (x, y + 14))
    ImageDraw.Draw(sheet).text((x + 4, y + 1), '%.0f s (guest %.1f)' % (t, gm / 1000), fill=(255, 255, 0))
sheet.save(sys.argv[3])
print(sys.argv[3], len(ts), 'cells')
