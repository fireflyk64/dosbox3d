#!/usr/bin/env python3
"""pvsheet.py OUTDIR SHEET.png [NAME ...]: tools/pvshot.c's pictures, the card's beside pview's, a row an eye."""
import sys, glob, os
from PIL import Image, ImageDraw
out, dst = sys.argv[1], sys.argv[2]
names = sorted(set(os.path.basename(p)[:-9] for p in glob.glob(out + '/*-card.ppm')))
only = sys.argv[3:] 
if only: names = [n for n in names if any(o in n for o in only)]
W, H = 640, 400
sheet = Image.new('RGB', (W * 2 + 8, (H + 18) * len(names)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    a = Image.open(f'{out}/{n}-card.ppm'); b = Image.open(f'{out}/{n}-pv.ppm')
    y = i * (H + 18)
    sheet.paste(a, (0, y + 18)); sheet.paste(b, (W + 8, y + 18))
    d.text((4, y + 3), n + '  card (terrain.c, 640x400)', fill=(255, 255, 255))
    d.text((W + 12, y + 3), n + '  pview (bake, 128x80)', fill=(255, 255, 255))
sheet.save(dst)
print(dst, sheet.size)
