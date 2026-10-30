#!/usr/bin/env python3
"""Build the app icon and LiveArea images from shapes and text (no game characters or logos - copyright, ADR-022).

Output (sources):
- vita/sce_sys_src/icon0.png (128x128), bg.png (840x500), startup.png (280x158)
  → python vita/tools/prepare_sce_sys.py writes them to vita/sce_sys/ in Vita format (8-bit palette)
- psp/sce_sys/ICON0.PNG (144x80). no PSP background (PIC1)
- art_psd/*.psd: same images split into layers (background, shadow, Rei, REM, underline, subtitle) for editing (only with psd-tools,
  not committed - .gitignore)
Usage (Pillow, optional psd-tools): python tools/make_app_art.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG0 = (14, 18, 40)      # top
BG1 = (40, 16, 56)      # bottom
GRID = (60, 90, 170)
ACCENT = (240, 144, 32)
TEXT = (255, 255, 255)
SUB = (200, 210, 240)

FONT_CANDIDATES = [
    "C:/Windows/Fonts/ariblk.ttf", "C:/Windows/Fonts/arialbd.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def font(size):
    for f in FONT_CANDIDATES:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def background(w, h):
    im = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):  # vertical gradient
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(BG0[i] * (1 - t) + BG1[i] * t) for i in range(3)))
    horizon = int(h * 0.62)  # perspective grid (lower part)
    for k in range(1, 12):
        y = horizon + int((h - horizon) * (k / 11) ** 1.8)
        d.line([(0, y), (w, y)], fill=GRID)
    for k in range(-12, 13):
        x0 = w / 2 + k * w / 24
        d.line([(w / 2 + k * w / 200, horizon), (x0 * 1.0 + k * w / 10, h)], fill=GRID)
    d.line([(0, horizon), (w, horizon)], fill=ACCENT, width=max(1, h // 120))
    for y in range(0, h, 6):  # scanlines
        d.line([(0, y), (w, y)], fill=(0, 0, 0), width=1)
    return im


class Art:
    """Layer set: (name, full-size RGBA). PNG is flattened, PSD keeps layers."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.layers = [("Background", background(w, h))]

    def layer(self, name):
        im = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        self.layers.append((name, im))
        return ImageDraw.Draw(im)

    def flat(self):
        out = self.layers[0][1].copy()
        for _, im in self.layers[1:]:
            out.alpha_composite(im)
        return out.convert("RGB")

    def save(self, png, psd_name):
        self.flat().save(png)
        try:
            from psd_tools import PSDImage
            from psd_tools.api.layers import PixelLayer
        except ImportError:
            return
        pd = os.path.join(ROOT, "art_psd")
        os.makedirs(pd, exist_ok=True)
        psd = PSDImage.new("RGB", (self.w, self.h))
        for name, im in self.layers:
            psd.append(PixelLayer.frompil(im, psd, name, 0, 0))
        psd.save(os.path.join(pd, psd_name))


def ink(text, f):
    """Actual ink box of the text (x0, y0, x1, y1), relative to origin."""
    return ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=f)


def shadowed(art, name, xy, text, f, fill):
    sd = art.layer(name + " shadow")
    for dx, dy in ((3, 3), (2, 2)):
        sd.text((xy[0] + dx, xy[1] + dy), text, font=f, fill=(0, 0, 0, 255))
    art.layer(name).text(xy, text, font=f, fill=fill)


def icon(art, size_top, size_bot, gap):
    """Rei (white) on top, REM (orange) below. Whole ink box of both lines (incl. underline) centered in the image."""
    ft, fb = font(size_top), font(size_bot)
    bt, bb = ink("Rei", ft), ink("REM", fb)
    lw = max(2, size_top // 12)
    th = bt[3] - bt[1]
    bh = bb[3] - bb[1]
    total = th + gap + lw + gap + bh
    top = (art.h - total) / 2
    xt = (art.w - (bt[2] - bt[0])) / 2 - bt[0]
    shadowed(art, "Rei", (xt, top - bt[1]), "Rei", ft, TEXT)
    ul = top + th + gap
    art.layer("Underline").line([(xt + bt[0], ul + lw / 2), (xt + bt[2], ul + lw / 2)], fill=ACCENT, width=lw)
    xb = (art.w - (bb[2] - bb[0])) / 2 - bb[0]
    shadowed(art, "REM", (xb, ul + lw + gap - bb[1]), "REM", fb, ACCENT)


def banner(art, size, cy, sub=None, sub_size=0):
    """Single line "ReiREM" (Rei white + REM orange), underline, subtitle. Text ink centered at cy."""
    f = font(size)
    w1 = ImageDraw.Draw(Image.new("L", (1, 1))).textlength("Rei", font=f)
    b = ink("ReiREM", f)
    x = (art.w - (b[2] - b[0])) / 2 - b[0]
    y = cy - (b[1] + b[3]) / 2
    sd = art.layer("Title shadow")
    for dx, dy in ((3, 3), (2, 2)):
        sd.text((x + dx, y + dy), "ReiREM", font=f, fill=(0, 0, 0, 255))
    art.layer("Rei").text((x, y), "Rei", font=f, fill=TEXT)
    art.layer("REM").text((x + w1, y), "REM", font=f, fill=ACCENT)
    lw = max(2, size // 12)
    uy = y + b[3] + max(3, size // 10)
    art.layer("Underline").line([(x + b[0], uy), (x + b[2], uy)], fill=ACCENT, width=lw)
    if sub:
        fs = font(sub_size)
        sb = ink(sub, fs)
        art.layer("Subtitle").text(((art.w - (sb[2] - sb[0])) / 2 - sb[0], uy + lw + max(4, size // 8) - sb[1]),
                                   sub, font=fs, fill=SUB)


def main():
    vs = os.path.join(ROOT, "vita", "sce_sys_src")
    os.makedirs(vs, exist_ok=True)
    a = Art(128, 128)
    icon(a, 44, 30, 6)
    a.save(os.path.join(vs, "icon0.png"), "vita_icon0.psd")
    a = Art(840, 500)
    banner(a, 120, 150, "arcade emulator", 34)
    a.save(os.path.join(vs, "bg.png"), "vita_bg.psd")
    a = Art(280, 158)
    banner(a, 52, 62, "START", 20)
    a.save(os.path.join(vs, "startup.png"), "vita_startup.psd")

    pd = os.path.join(ROOT, "psp", "sce_sys")
    os.makedirs(pd, exist_ok=True)
    a = Art(144, 80)
    banner(a, 30, 34)
    a.save(os.path.join(pd, "ICON0.PNG"), "psp_icon0.psd")
    print("ok")


if __name__ == "__main__":
    main()
