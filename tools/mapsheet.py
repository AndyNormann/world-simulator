#!/usr/bin/env python3
"""Tile world maps into one contact sheet with seed labels.

    tools/mapsheet.py OUT.png build/maps/world-*.png

Ordering is by seed, extracted from the filename (world-17.png -> 17)."""
import sys
from PIL import Image, ImageDraw

out, paths = sys.argv[1], sys.argv[2:]
if not paths:
    sys.exit("usage: mapsheet.py OUT.png world-*.png")

def seed_of(p):
    d = "".join(c for c in p.rsplit("/", 1)[-1] if c.isdigit())
    return int(d) if d else 0

paths.sort(key=seed_of)
cols = 8
rows = (len(paths) + cols - 1) // cols
cell = 240
pad = 4
label = 14
sheet = Image.new("RGB", (cols * (cell + pad) + pad, rows * (cell + pad + label) + pad), (18, 18, 20))
draw = ImageDraw.Draw(sheet)
for i, p in enumerate(paths):
    im = Image.open(p).convert("RGB").resize((cell, cell), Image.LANCZOS)
    x = pad + (i % cols) * (cell + pad)
    y = pad + (i // cols) * (cell + pad + label)
    sheet.paste(im, (x, y + label))
    draw.text((x + 2, y + 1), "seed %d" % seed_of(p), fill=(220, 220, 220))
sheet.save(out)
print("sheet %s  %dx%d  %d worlds" % (out, sheet.width, sheet.height, len(paths)))
