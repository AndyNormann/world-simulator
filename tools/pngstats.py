#!/usr/bin/env python3
"""Dump basic stats about screenshot.png: unique colors, top colors, coverage."""
import zlib, struct, sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else 'screenshot.png'
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
i, cnt = 0, Counter()

def unfilter(ftype, line, prev, bpp):
    for x in range(len(line)):
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
    return line

for _ in range(h):
    ftype = raw[i]; i += 1
    line = unfilter(ftype, bytearray(raw[i:i+stride]), prev, bpp); i += stride
    for x in range(0, stride, bpp*13):
        cnt[bytes(line[x:x+3])] += 1
    prev = line

total = sum(cnt.values())
print(f"{path}: {w}x{h}  sampled colors={len(cnt)}")
for col, n in cnt.most_common(8):
    print(f"  #{col[0]:02x}{col[1]:02x}{col[2]:02x}  {100*n/total:5.1f}%")
