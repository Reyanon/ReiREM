#!/usr/bin/env python3
# Analyze a GE screen dump (psp/src/video.c video_dump: 320x240 RGBA + reference indices + palette 0x801).
# For each wrong pixel, count which reference pixel offset (dx, dy) has the same color to find the shape of the misalignment.
#   python psp/tools/ge_diff.py build-psp/ge_dump.bin [out.png]
import struct
import sys
from collections import Counter

W, H = 320, 240
d = open(sys.argv[1], 'rb').read()
img = struct.unpack('<%dI' % (W * H), d[:W * H * 4])
idx = struct.unpack('<%dH' % (W * H), d[W * H * 4:W * H * 6])
pal = struct.unpack('<%dI' % 0x801, d[W * H * 6:W * H * 6 + 0x801 * 4])


def ex(c):
    return (c << 3) | (c >> 2)


def col(i):
    c = pal[i]
    return ex(c & 31) | (ex((c >> 5) & 31) << 8) | (ex((c >> 10) & 31) << 16)


want = [col(i) for i in idx]
got = [p & 0xffffff for p in img]
bad = [i for i in range(W * H) if want[i] != got[i]]
print('differing pixels', len(bad))
hist = Counter()
for i in bad[:20000]:
    x, y = i % W, i // W
    for dy in range(-8, 9):
        for dx in range(-8, 9):
            xx, yy = x + dx, y + dy
            if 0 <= xx < W and 0 <= yy < H and want[yy * W + xx] == got[i]:
                hist[(dx, dy)] += 1
print('matching offset (dx,dy) top:', hist.most_common(12))
rows = Counter(i // W for i in bad)
cols = Counter(i % W for i in bad)
print('top rows:', sorted(rows.items())[:10], '...')
print('col % 8 distribution:', sorted(Counter((i % W) % 8 for i in bad).items()))
print('row % 8 distribution:', sorted(Counter((i // W) % 8 for i in bad).items()))
if len(sys.argv) > 2:
    from PIL import Image
    im = Image.new('RGB', (W * 3, H))
    px = im.load()
    for i in range(W * H):
        x, y = i % W, i // W
        for k, c in enumerate((want[i], got[i], 0xffffff if want[i] != got[i] else 0)):
            px[x + k * W, y] = (c & 255, (c >> 8) & 255, (c >> 16) & 255)
    im.resize((W * 6, H * 2), 0).save(sys.argv[2])
