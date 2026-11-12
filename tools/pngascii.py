#!/usr/bin/env python3
"""Downsample screenshot.png to ASCII so the render can be judged in a terminal.

Characters classify by colour family rather than brightness:
  ~ water   . sand   , grass   * rock   # snow   (space) sky
"""
import zlib, struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'screenshot.png'
COLS = int(sys.argv[2]) if len(sys.argv) > 2 else 100

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
rows = []
i = 0
for _ in range(h):
    ftype = raw[i]; i += 1
    line = bytearray(raw[i:i+stride]); i += stride
    for x in range(stride):
        a = line[x-bpp] if x >= bpp else 0
        b = prev[x]
        c = prev[x-bpp] if x >= bpp else 0
        if ftype == 1: line[x] = (line[x] + a) & 255
        elif ftype == 2: line[x] = (line[x] + b) & 255
        elif ftype == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif ftype == 4:
            p = a + b - c
            pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[x] = (line[x] + pr) & 255
    rows.append(bytes(line)); prev = line

ROWS = max(1, int(COLS * h / w * 0.5))

def classify(r, g, b):
    if b > 150 and b > r + 40 and b > g + 20 and abs(g - 191) < 60 and r > 80:
        return ' '                      # sky
    if b > r and b > g and b < 200:
        return '~'                      # water
    if r > 150 and g > 130 and b > 90:
        return '.'                      # sand
    if g > r + 20 and g > b + 20:
        return ','                      # grass
    if r > 180 and g > 180 and b > 180:
        return '#'                      # snow
    return '*'

out = []
for ry in range(ROWS):
    y0, y1 = ry*h//ROWS, max(ry*h//ROWS + 1, (ry+1)*h//ROWS)
    line = []
    for rx in range(COLS):
        x0, x1 = rx*w//COLS, max(rx*w//COLS + 1, (rx+1)*w//COLS)
        tr = tg = tb = n = 0
        for y in range(y0, y1, max(1, (y1-y0)//4)):
            row = rows[y]
            for x in range(x0, x1, max(1, (x1-x0)//4)):
                o = x*bpp
                tr += row[o]; tg += row[o+1]; tb += row[o+2]; n += 1
        line.append(classify(tr//n, tg//n, tb//n))
    out.append(''.join(line))
print('\n'.join(out))
