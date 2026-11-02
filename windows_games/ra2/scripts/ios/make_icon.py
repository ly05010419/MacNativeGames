#!/usr/bin/env python3
"""Draws the iOS app icon (1024x1024 PNG): original artwork, no game assets
or publisher logos. A red star on a dark steel background with "RA2".

    python3 -I scripts/ios/make_icon.py src/ios/AppIcon.png
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 1024
FONTS = (
    "/System/Library/Fonts/Supplemental/Arial Black.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
)


def star(cx, cy, r_out, r_in, points=5):
    pts = []
    for i in range(points * 2):
        r = r_out if i % 2 == 0 else r_in
        a = -math.pi / 2 + i * math.pi / points
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def font(size):
    for path in FONTS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    sys.exit("make_icon: no usable bold font found")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    img = Image.new("RGB", (SIZE, SIZE))
    d = ImageDraw.Draw(img)
    # Background: dark steel, lighter towards the top.
    for y in range(SIZE):
        t = y / (SIZE - 1)
        v = int(58 - 40 * t)
        d.line([(0, y), (SIZE, y)], fill=(v + 6, v, v))
    # A soft red glow behind the star.
    glow = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(glow).ellipse((170, 90, 854, 774), fill=170)
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    img.paste((150, 10, 10), (0, 0), glow)
    d = ImageDraw.Draw(img)
    # The star: a gold rim around a red face.
    cx, cy = SIZE // 2, 420
    d.polygon(star(cx, cy, 330, 135), fill=(232, 182, 60))
    d.polygon(star(cx, cy, 292, 118), fill=(206, 24, 24))
    # Its upper-left facets catch the light.
    face = star(cx, cy, 292, 118)
    for i in range(0, 10, 2):
        d.polygon([(cx, cy), face[i], face[(i + 1) % 10]], fill=(232, 52, 40))
    # The title.
    f = font(230)
    text = "RA2"
    box = d.textbbox((0, 0), text, font=f)
    x = (SIZE - (box[2] - box[0])) // 2 - box[0]
    y = 930 - box[3]
    d.text((x + 8, y + 10), text, font=f, fill=(0, 0, 0))
    d.text((x, y), text, font=f, fill=(240, 236, 226))
    img.save(sys.argv[1])


if __name__ == "__main__":
    main()
