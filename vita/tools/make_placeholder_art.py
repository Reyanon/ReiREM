#!/usr/bin/env python3
"""sce_sys placeholder image generator (standard library only).

Vita LiveArea images must be 8-bit indexed PNGs or installation fails.
Solid color + border images are used until replaced with real art in M5.

Usage: python tools/make_placeholder_art.py  (from the repo root)
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# palette: 0=background (navy), 1=border (orange)
PALETTE = [(0x20, 0x30, 0x80), (0xF0, 0x90, 0x20)]

TARGETS = [
    ("sce_sys/icon0.png", 128, 128, 6),
    ("sce_sys/livearea/contents/bg.png", 840, 500, 12),
    ("sce_sys/livearea/contents/startup.png", 280, 158, 6),
]


def chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def indexed_png(w, h, border):
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # no filter
        edge_y = y < border or y >= h - border
        for x in range(w):
            rows.append(1 if edge_y or x < border or x >= w - border else 0)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)  # 8-bit, color type 3 (indexed)
    plte = b"".join(bytes(c) for c in PALETTE)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte)
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def main():
    for rel, w, h, border in TARGETS:
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(indexed_png(w, h, border))
        print(f"{rel}: {w}x{h}")


if __name__ == "__main__":
    main()
