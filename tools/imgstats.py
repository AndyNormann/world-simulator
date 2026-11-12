#!/usr/bin/env python3
"""Compare renders against the reference look, numerically.

    python3 tools/imgstats.py screenshot.png ~/Downloads/inspo.jpg

Reference targets (measured from the inspo image):
    grass 28%  rock 25%  sky 28%  shadow 6%  sand 2%  water 1%
    terrain flat 73%  hard-edge 11%
    palette warm/desaturated: #c0c0a0 #a0c0a0 #e0c0a0 + near-black #200000

Requires Pillow (JPEG support). PNG-only fallback is tools/pngstats.py.
"""
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    sys.exit("imgstats: needs Pillow (pip install pillow)")

TARGET = {
    "grass": 28, "rock": 25, "sky": 28, "shadow": 6, "sand": 2, "water": 1,
}
TARGET_FLAT = 73
TARGET_EDGE = 11


def hsv(r, g, b):
    mx, mn = max(r, g, b), min(r, g, b)
    v = mx / 255.0
    d = mx - mn
    s = 0.0 if mx == 0 else d / mx
    if d == 0:
        h = 0.0
    elif mx == r:
        h = (60 * ((g - b) / d)) % 360
    elif mx == g:
        h = 60 * ((b - r) / d) + 120
    else:
        h = 60 * ((r - g) / d) + 240
    return h, s, v


def family(r, g, b):
    h, s, v = hsv(r, g, b)
    if v < 0.16:
        return "shadow"
    if v > 0.62 and s < 0.30:
        return "sky"
    if 55 <= h <= 110 and s > 0.35:
        return "grass"
    if 15 <= h <= 45 and s > 0.35:
        return "rock"
    if 170 <= h <= 260 and s > 0.30:
        return "water"
    if 40 <= h < 60 and s <= 0.35:
        return "sand"
    return "other"


def lum(p, x, y):
    r, g, b = p[x, y]
    return (0.299 * r + 0.587 * g + 0.114 * b) / 255.0


def is_sky(p, x, y):
    r, g, b = p[x, y]
    h, s, v = hsv(r, g, b)
    return v > 0.62 and s < 0.30


# Reference measured off ~/Downloads/lowpolytrees.webp (1440x960): 36 blobs,
# median height 140px = 15% of frame, aspect w/h 0.69 [0.41..1.42], widest 217px,
# 11.5% of the frame green, canopy #758f50.
REF_TREES = {"blobs": 36, "h_pct": 15, "aspect": 0.69, "widest": 217, "green": 11.5}


def report_trees(path):
    """Tree silhouettes: same blob analysis on the reference and on a
    --trees-only render (grey ground, real trees), so the two are comparable."""
    im = Image.open(path).convert("RGB")
    W, H = im.size
    p = im.load()
    step = max(1, W // 400)

    def green(x, y):
        h, s, v = hsv(*p[x, y])
        return 70 <= h <= 170 and v > 0.30

    cols, rows = len(range(0, W, step)), len(range(0, H, step))
    m = [[green(x * step, y * step) for x in range(cols)] for y in range(rows)]
    seen = [[False] * cols for _ in range(rows)]
    blobs = []
    for y in range(rows):
        for x in range(cols):
            if not m[y][x] or seen[y][x]:
                continue
            stack = [(x, y)]
            seen[y][x] = True
            pts = []
            while stack:
                cx, cy = stack.pop()
                pts.append((cx, cy))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = cx + dx, cy + dy
                    if 0 <= nx < cols and 0 <= ny < rows and m[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            if len(pts) >= 12:
                blobs.append(pts)

    print("%-34s %dx%d  trees" % (path.split("/")[-1], W, H))
    if not blobs:
        print("  no green blobs found")
        print()
        return
    boxes = []
    for b in blobs:
        xs = [q[0] for q in b]
        ys = [q[1] for q in b]
        boxes.append((min(xs) * step, min(ys) * step,
                      (max(xs) - min(xs) + 1) * step, (max(ys) - min(ys) + 1) * step))
    areas = sorted((w * h for _, _, w, h in boxes))
    heights = sorted(h for _, _, _, h in boxes)
    aspects = sorted(w / max(1.0, h) for _, _, w, h in boxes)
    med = lambda v: v[len(v) // 2]
    print("  blobs %d/%d  height median %dpx (%.0f%% of frame, ref %d%%)  widest %dpx/%d"
          % (len(blobs), REF_TREES["blobs"], med(heights), 100.0 * med(heights) / H,
             REF_TREES["h_pct"], max(w for _, _, w, _ in boxes), REF_TREES["widest"]))
    print("  aspect w/h median %.2f [p10 %.2f p90 %.2f]  ref %.2f"
          % (med(aspects), aspects[len(aspects) // 10], aspects[9 * len(aspects) // 10],
             REF_TREES["aspect"]))
    print("  area median %dpx2  green %.1f%% (ref %.1f%%)"
          % (med(areas), 100.0 * sum(len(b) for b in blobs) / (cols * rows), REF_TREES["green"]))

    q = Counter()
    for b in blobs:
        for (x, y) in b[::7]:
            r, g, bb = p[min(x * step, W - 1), min(y * step, H - 1)]
            q[(r // 16 * 16, g // 16 * 16, bb // 16 * 16)] += 1
    print("  canopy: " + " ".join("#%02x%02x%02x" % c for c, _ in q.most_common(5))
          + "  ref #758f50")
    print()


def report(path, map_cols=0):
    im = Image.open(path).convert("RGB")
    W, H = im.size
    p = im.load()

    fams = Counter()
    for y in range(0, H, 3):
        for x in range(0, W, 3):
            fams[family(*p[x, y])] += 1
    tot = sum(fams.values())
    fam = {k: 100.0 * v / tot for k, v in fams.items()}

    buckets = [0, 0, 0, 0]
    n = 0
    for y in range(0, H, 2):
        for x in range(0, W - 2, 2):
            if is_sky(p, x, y) or is_sky(p, x + 2, y):
                continue
            d = abs(lum(p, x + 2, y) - lum(p, x, y))
            buckets[min(3, int(d * 5 * 4))] += 1
            n += 1
    flat = 100.0 * buckets[0] / max(1, n)
    edge = 100.0 * buckets[3] / max(1, n)

    q = Counter()
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            r, g, b = p[x, y]
            q[(r // 32 * 32, g // 32 * 32, b // 32 * 32)] += 1
    qt = sum(q.values())

    print("%-34s %dx%d" % (path.split("/")[-1], W, H))
    row = []
    for k in sorted(TARGET, key=lambda k: -TARGET[k]):
        got = fam.get(k, 0.0)
        row.append("%s %2.0f%%/%d%%" % (k, got, TARGET[k]))
    print("  " + "  ".join(row))
    print("  other %.0f%%  flat %.0f%%/%d%%  hard-edge %.0f%%/%d%%"
          % (fam.get("other", 0.0), flat, TARGET_FLAT, edge, TARGET_EDGE))
    print("  palette: " + " ".join("#%02x%02x%02x(%.0f%%)" % (r, g, b, 100.0 * k / qt)
                                   for (r, g, b), k in q.most_common(6)))
    if map_cols:
        print("  family map:")
        rows = max(1, int(map_cols * H / W * 0.5))
        for ry in range(rows):
            line = []
            for rx in range(map_cols):
                x = int((rx + 0.5) * W / map_cols)
                y = int((ry + 0.5) * H / rows)
                f = family(*p[x, y])
                line.append({"shadow": "k", "sky": " ", "grass": ",",
                             "rock": "*", "water": "~", "sand": ".", "other": "?"}[f])
            print("  |" + "".join(line) + "|")
    print()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    cols = 0
    trees = False
    if args and args[0] == "-m":
        cols = 78
        args = args[1:]
    if args and args[0] == "--trees":
        trees = True
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    for a in args:
        if trees:
            report_trees(a)
        else:
            report(a, cols)
