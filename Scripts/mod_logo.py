"""Skyward's logo for ficsit.app: M-Bot flying through a glowing acclivity ring against a starry sky.

Needs Pillow. Usage: python mod_logo.py OUT.png [size]
"""
import math
import os
import random
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import mbot_icon


def font(size):
    for name in ("bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf"):
        try:
            f = ImageFont.truetype(name, size)
            try:
                f.set_variation_by_name("Bold")  # Bahnschrift is a variable font
            except Exception:
                pass
            return f
        except OSError:
            continue
    return ImageFont.load_default()


def render(out_path, size=512):
    S = size
    rnd = random.Random(7)

    # Night sky: deep blue at the top fading to near-black.
    img = Image.new("RGB", (S, S))
    px = img.load()
    for y in range(S):
        t = y / (S - 1)
        r, g, b = int(8 + 6 * (1 - t)), int(14 + 22 * (1 - t)), int(34 + 50 * (1 - t))
        for x in range(S):
            px[x, y] = (r, g, b)
    img = img.convert("RGBA")

    stars = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(stars)
    for _ in range(int(S * 0.45)):
        x, y = rnd.random() * S, rnd.random() * S * 0.8
        rad = rnd.choice((0.6, 0.8, 1.0, 1.4)) * S / 512
        a = rnd.randint(90, 230)
        d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=(220, 235, 255, a))
    img = Image.alpha_composite(img, stars)

    # The acclivity ring, seen at an angle, glowing.
    cx, cy = S * 0.5, S * 0.47
    rx, ry = S * 0.40, S * 0.16
    width = max(2, int(S * 0.028))
    box = (cx - rx, cy - ry, cx + rx, cy + ry)

    def ring_layer(start, end):
        layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        for blur, colour, w in ((S * 0.05, (40, 140, 255, 200), width * 3), (S * 0.012, (90, 190, 255, 255), width * 1.6), (0, (210, 240, 255, 255), width)):
            part = Image.new("RGBA", (S, S), (0, 0, 0, 0))
            ImageDraw.Draw(part).arc(box, start, end, fill=colour, width=int(w))
            if blur:
                part = part.filter(ImageFilter.GaussianBlur(blur))
            layer = Image.alpha_composite(layer, part)
        return layer

    # Back half of the ring behind the ship, front half over it.
    img = Image.alpha_composite(img, ring_layer(180, 360))

    tmp = os.path.join(tempfile.gettempdir(), "skyward_logo_mbot.png")
    mbot_icon.render_icon(tmp, size=S, supersample=2, yaw_deg=-120.0, pitch_deg=32.0)
    ship = Image.open(tmp).convert("RGBA")
    # Drop the icon's own soft halo: keep only the ship (near-opaque pixels).
    alpha = ship.getchannel("A").point(lambda a: 255 if a > 200 else int(a * a / 200 / 200 * 255) if a > 120 else 0)
    ship.putalpha(alpha)
    ship = ship.resize((int(S * 0.82), int(S * 0.82)), Image.LANCZOS)
    glow = Image.new("RGBA", ship.size, (60, 150, 255, 0))
    glow.putalpha(ship.getchannel("A").filter(ImageFilter.GaussianBlur(S * 0.02)).point(lambda a: int(a * 0.55)))
    ox, oy = int(cx - ship.width / 2), int(cy - ship.height / 2 - S * 0.03)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    layer.paste(glow, (ox, oy), glow)
    img = Image.alpha_composite(img, layer)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    layer.paste(ship, (ox, oy), ship)
    img = Image.alpha_composite(img, layer)

    img = Image.alpha_composite(img, ring_layer(0, 180))

    # Title.
    title = "SKYWARD"
    f = font(int(S * 0.15))
    td = ImageDraw.Draw(img)
    tw = td.textlength(title, font=f)
    tx, ty = (S - tw) / 2, S * 0.78
    halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(halo).text((tx, ty), title, font=f, fill=(60, 160, 255, 255))
    img = Image.alpha_composite(img, halo.filter(ImageFilter.GaussianBlur(S * 0.018)))
    ImageDraw.Draw(img).text((tx, ty), title, font=f, fill=(235, 245, 255, 255))

    img.convert("RGB").save(out_path)
    return out_path


if __name__ == "__main__":
    render(sys.argv[1] if len(sys.argv) > 1 else "Skyward-logo.png", int(sys.argv[2]) if len(sys.argv) > 2 else 512)
