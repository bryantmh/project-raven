#!/usr/bin/env python3
"""Draw Project Raven's icon: a ring world in orbit around a planet, with a raven crossing it (original art in the
style of the Halo setting; no game logos). Writes assets/raven.ico (16-256 px) and assets/raven.png (preview).

  python tools/make_icon.py
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # drawn at 4x and scaled down
OUT = os.path.join(os.path.dirname(__file__), "..", "assets")


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius, fill=255)
    return m


def background():
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        for x in range(S):
            d = math.hypot(x - S * 0.62, y - S * 0.35) / S
            t = min(1.0, d * 1.3)
            px[x, y] = (int(18 * (1 - t) + 3 * t), int(40 * (1 - t) + 6 * t), int(78 * (1 - t) + 16 * t))
    d = ImageDraw.Draw(img)
    rnd = 7
    for i in range(140):  # stars
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        x = rnd % S
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        y = rnd % S
        r = 1 + (i % 3)
        b = 140 + (i * 37) % 115
        d.ellipse([x - r, y - r, x + r, y + r], fill=(b, b, min(255, b + 30)))
    return img


def planet(img):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    cx, cy, r = S * 0.70, S * 0.28, S * 0.17
    for i in range(int(r), 0, -2):  # shaded gas giant, lit from the left
        t = i / r
        col = (int(150 + 60 * (1 - t)), int(110 + 50 * (1 - t)), int(90 + 40 * (1 - t)), 255)
        d.ellipse([cx - (1 - t) * r * 0.3 - i, cy - i, cx - (1 - t) * r * 0.3 + i, cy + i], fill=col)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    img.paste(layer, (0, 0), mask)


def ellipse_pts(cx, cy, a, b, tilt, d0, d1, step=1):
    out = []
    for deg in range(d0, d1 + 1, step):
        t = math.radians(deg)
        x, y = a * math.cos(t), b * math.sin(t)
        out.append((cx + x * math.cos(tilt) - y * math.sin(tilt), cy + x * math.sin(tilt) + y * math.cos(tilt)))
    return out


def ring(img, front):
    """The ring world seen from slightly above: a wide band whose inner surface (land, sea, sky) faces us on the
    far side. The far half is drawn first, the near half (just the steel outer hull) last."""
    cx, cy, a, b, tilt, w = S * 0.47, S * 0.60, S * 0.47, S * 0.17, math.radians(-14), S * 0.075
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    if not front:
        top = ellipse_pts(cx, cy - w, a, b, tilt, 180, 360)
        bot = ellipse_pts(cx, cy, a, b, tilt, 180, 360)
        glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(glow).polygon(top + bot[::-1], fill=(90, 200, 255, 160))
        g = glow.filter(ImageFilter.GaussianBlur(18))
        img.paste(g, (0, 0), g)
        # inner surface: sky blue at the rim, green land, a darker sea stripe
        for k, col in ((0.0, (150, 210, 245)), (0.35, (70, 150, 90)), (0.55, (40, 110, 150)), (0.7, (80, 150, 80))):
            t0 = ellipse_pts(cx, cy - w * (1 - k), a, b, tilt, 180, 360)
            d.polygon(t0 + bot[::-1], fill=col + (255,))
        d.line(top, fill=(225, 240, 250, 255), width=10)
    else:
        top = ellipse_pts(cx, cy - w, a, b, tilt, 0, 180)
        bot = ellipse_pts(cx, cy, a, b, tilt, 0, 180)
        d.polygon(top + bot[::-1], fill=(170, 185, 200, 255))
        d.line(top, fill=(230, 240, 250, 255), width=8)
        d.line(bot, fill=(90, 105, 125, 255), width=8)
        for deg in range(10, 180, 14):  # hull panel seams
            p0 = ellipse_pts(cx, cy - w, a, b, tilt, deg, deg)[0]
            p1 = ellipse_pts(cx, cy, a, b, tilt, deg, deg)[0]
            d.line([p0, p1], fill=(120, 135, 155, 255), width=4)
    img.paste(layer, (0, 0), layer)


def raven(img):
    """A raven gliding toward the viewer, wings spread with fingered primaries, wedge tail."""
    k = S / 1024
    cx, cy = S * 0.46, S * 0.40
    half = [(0, -70), (14, -60), (20, -40), (40, -30), (120, -55), (220, -95), (330, -120), (300, -95), (345, -92),
            (305, -70), (340, -58), (290, -45), (315, -28), (250, -22), (170, 0), (90, 20), (40, 30), (28, 70),
            (45, 120), (0, 105)]
    right = [(cx + x * k, cy + y * k) for x, y in half]
    left = [(cx - x * k, cy + y * k) for x, y in reversed(half)]
    bird = right + left
    head = [(cx - 16 * k, cy - 66 * k), (cx, cy - 92 * k), (cx + 16 * k, cy - 66 * k)]  # beak toward the viewer
    edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ed = ImageDraw.Draw(edge)
    ed.polygon(bird, outline=(110, 215, 255, 255), width=14)
    ed.polygon(head, outline=(110, 215, 255, 255), width=10)
    e = edge.filter(ImageFilter.GaussianBlur(8))
    img.paste(e, (0, 0), e)
    d = ImageDraw.Draw(img)
    d.polygon(bird, fill=(8, 10, 16))
    d.polygon(head, fill=(8, 10, 16))
    for sx in (-1, 1):
        d.ellipse([cx + sx * 11 * k - 5 * k, cy - 58 * k, cx + sx * 11 * k + 5 * k, cy - 48 * k], fill=(120, 220, 255))


def main():
    img = background()
    planet(img)
    ring(img, front=False)
    ring(img, front=True)
    raven(img)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), rounded_mask(S, S // 6))
    os.makedirs(OUT, exist_ok=True)
    big = out.resize((256, 256), Image.LANCZOS)
    big.save(os.path.join(OUT, "raven.png"))
    big.save(os.path.join(OUT, "raven.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", os.path.join(OUT, "raven.ico"))


if __name__ == "__main__":
    main()
