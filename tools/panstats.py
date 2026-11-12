#!/usr/bin/env python3
"""Compare renders across a camera pan.

Prints, per image: sky fraction, horizontal centroid of non-sky pixels, and the
mean screen height of the terrain silhouette's top edge. A correct pan moves the
centroid horizontally while the silhouette height stays roughly constant; if the
height swings, the world is tilting (which reads as rotation).
"""
import zlib, struct, sys

def load(path):
    d = open(path, 'rb').read()
    pos, w, h, ct, idat = 8, None, None, None, b''
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        typ, data = d[pos+4:pos+8], d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', data[:10])
        elif typ == b'IDAT':
            idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(h):
        ft = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x >= bpp else 0
            if ft == 1: line[x] = (line[x] + a) & 255
            elif ft == 2: line[x] = (line[x] + b) & 255
            elif ft == 3: line[x] = (line[x] + (a + b)//2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows

def is_sky(r, g, b):
    # clear colour and its fog blend: blue-dominant, bright
    return b > 170 and b > r + 30 and g > 120

for path in sys.argv[1:]:
    w, h, bpp, rows = load(path)
    total = sx = n = 0
    tops = []
    for y in range(h):
        row = rows[y]
        first = None
        for x in range(0, w, 4):
            o = x*bpp
            total += 1
            if not is_sky(row[o], row[o+1], row[o+2]):
                n += 1
                sx += x
                if first is None:
                    first = y
        if first is not None:
            tops.append(first)
    print(f"{path}: {w}x{h} sky={100*(total-n)/total:5.1f}% "
          f"terrain_cx={sx/max(n,1)/w:6.3f} top_edge_y={(sum(tops)/max(len(tops),1))/h:6.3f}")
