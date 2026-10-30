# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Generates res/app.ico: the "Horizon Sun" logo (v0.10) — a bright sun with a
# white hot core over a dark horizon, on a blue rounded tile. Each icon size
# is drawn on its own (8x supersampled) so even 16 px stays crisp.
# Run: python3 tools/make_icon.py
import os
from PIL import Image, ImageDraw

out_dir = os.path.join(os.path.dirname(__file__), "..", "res")
os.makedirs(out_dir, exist_ok=True)

BLUE = (0x2f, 0x6f, 0xd6, 255)
GROUND = (0x13, 0x2f, 0x5a, 255)
GLOW = (0xff, 0xd2, 0x7a)
SUN = (0xff, 0xc2, 0x5a, 255)
CORE = (0xff, 0xff, 0xff, 255)


def draw(size):
    """The logo at `size` px, designed on a 64-unit grid."""
    ss = 8
    S = size * ss
    u = S / 64.0
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    tile = Image.new("RGBA", (S, S), BLUE)
    d = ImageDraw.Draw(tile)
    # Glow (30 %), sun, white core: centre (40, 27).
    cx, cy = 40 * u, 27 * u
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([cx - 14 * u, cy - 14 * u, cx + 14 * u, cy + 14 * u], fill=GLOW + (77,))
    tile = Image.alpha_composite(tile, glow)
    d = ImageDraw.Draw(tile)
    d.ellipse([cx - 8.5 * u, cy - 8.5 * u, cx + 8.5 * u, cy + 8.5 * u], fill=SUN)
    d.ellipse([cx - 4 * u, cy - 4 * u, cx + 4 * u, cy + 4 * u], fill=CORE)
    # Dark ground below the horizon (y = 41).
    d.rectangle([0, 41 * u, S, S], fill=GROUND)
    # Rounded tile (radius 15 of 64).
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(15 * u), fill=255)
    img.paste(tile, (0, 0), mask)
    return img.resize((size, size), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
frames = [draw(s) for s in sizes]
big = frames[-1]
big.save(os.path.join(out_dir, "app.ico"), sizes=[(s, s) for s in sizes], append_images=frames[:-1])
draw(512).save(os.path.join(out_dir, "app_icon_preview.png"))
print("wrote", os.path.join(out_dir, "app.ico"))
