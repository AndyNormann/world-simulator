#!/usr/bin/env python3
"""Mean per-channel difference between two same-size renders.

    python3 tools/diff.py A.png B.png [--tol 0.3] [--heat out.png]

Exit 0 below the tolerance, 1 above, 2 on a size mismatch. Tolerance is a
percentage of 255: 0.3 means "almost indistinguishable", 2.0 means "you can see
it in a flat area".
"""
import sys
from PIL import Image, ImageChops

def main(argv):
    args, tol, heat = [], 0.3, None
    it = iter(argv[1:])
    for a in it:
        if a == "--tol":
            tol = float(next(it))
        elif a == "--heat":
            heat = next(it)
        else:
            args.append(a)
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = args
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if ia.size != ib.size:
        print("diff: size mismatch %s %s vs %s %s" % (ia.size + ib.size))
        return 2
    d = ImageChops.difference(ia, ib)
    hist = d.histogram()
    n = ia.size[0] * ia.size[1]
    mean = sum(v * c for v, c in enumerate(hist[:256])) / n / 1.0
    # fraction of pixels that differ visibly at all (any channel > 8)
    mask = d.convert("L").point(lambda v: 255 if v > 8 else 0)
    changed = mask.histogram()[255] / n * 100.0
    print("diff: %s vs %s  mean=%.3f%%  pixels>8=%0.2f%%  tol=%.3f%%"
          % (a, b, mean, changed, tol))
    if heat:
        amp = d.convert("L").point(lambda v: min(255, v * 8))
        Image.merge("RGB", [amp, amp, amp]).save(heat)
        print("diff: heatmap -> %s" % heat)
    return 0 if mean <= tol else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv))
