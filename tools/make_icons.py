"""Generate tray icons for NX Control: a dot-matrix headphone glyph.

on.ico  = connected (white glyph, red accent dot)
off.ico = not connected (grey glyph)
"""
import sys
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def glyph(size: int, fg, accent):
    scale = 4  # supersample for smooth edges
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = max(2, int(s * 0.11))
    # Headband arc
    pad = int(s * 0.14)
    d.arc([pad, pad, s - pad, s - pad + int(s * 0.18)], start=180, end=360, fill=fg, width=w)
    # Ear cups
    cup_w, cup_h = int(s * 0.22), int(s * 0.36)
    top = int(s * 0.46)
    d.rounded_rectangle([pad - w // 2, top, pad - w // 2 + cup_w, top + cup_h], radius=int(s * 0.07), fill=fg)
    d.rounded_rectangle([s - pad + w // 2 - cup_w, top, s - pad + w // 2, top + cup_h], radius=int(s * 0.07), fill=fg)
    # Accent dot (Nothing-style red)
    if accent:
        r = int(s * 0.075)
        cx, cy = s // 2, int(s * 0.72)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=accent)
    return img.resize((size, size), Image.LANCZOS)


def save(path, fg, accent):
    base = glyph(256, fg, accent)
    frames = [glyph(sz, fg, accent) for sz in SIZES]
    base.save(path, format="ICO", sizes=[(f.width, f.height) for f in frames], append_images=frames)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    save(f"{out}/on.ico", (255, 255, 255, 255), (215, 25, 32, 255))
    save(f"{out}/off.ico", (140, 140, 140, 255), None)
