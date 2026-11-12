#!/usr/bin/env python3
"""Downscale a render so it fits in a model context.

    python3 tools/shrink.py shot.png [width]
"""
import os
import sys

from PIL import Image


def shrink(src, width=900):
    im = Image.open(src).convert("RGB")
    w, h = im.size
    if w > width:
        im = im.resize((width, max(1, int(h * width / w))), Image.LANCZOS)
    base = os.path.splitext(src)[0]
    png = base + ".small.png"
    jpg = base + ".small.jpg"
    im.save(png)
    im.save(jpg, quality=88)
    print("%s -> %s (%dx%d, %d KB) / %s (%d KB)"
          % (src, png, im.size[0], im.size[1], os.path.getsize(png) // 1024,
             jpg, os.path.getsize(jpg) // 1024))
    return png, jpg


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    shrink(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 900)
