# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Title and end cards for the EXRay demo video (1920x1080 PNG).
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import make_promo as mp   # reuses icon(), background(), fonts (this also rebuilds the promo images)
from PIL import ImageDraw
W, H = mp.W, mp.H
out = os.path.join(mp.P, "video")
os.makedirs(out, exist_ok=True)

def card(name, lines, warm_y):
    img = mp.background(0.5, warm_y)
    d = ImageDraw.Draw(img)
    ic = mp.icon(150)
    img.alpha_composite(ic, (W // 2 - 75, 250))
    y = 440
    for text, font, size, color, gap in lines:
        d.text((W // 2, y), text, font=mp.F(font, size), fill=color, anchor="mt")
        y += gap
    img.convert("RGB").save(os.path.join(out, name))
    print("wrote", name)

card("card_title.png", [
    ("EXRay", "InterDisplay-Bold.otf", 150, mp.TEXT, 190),
    ("The fast HDR & EXR viewer for 3D artists", "Inter-SemiBold.otf", 46, mp.TEXT, 80),
    ("Free  ·  Portable  ·  Windows 11", "Inter-Medium.otf", 34, mp.WARM, 0),
], 0.15)
card("card_end.png", [
    ("Download EXRay free", "InterDisplay-Bold.otf", 96, mp.TEXT, 130),
    ("p1xelrise.gumroad.com/l/exray", "Inter-SemiBold.otf", 50, mp.ACCENT, 90),
    ("One .exe  ·  nothing to install  ·  works offline  ·  beta", "Inter-Medium.otf", 32, mp.DIM, 70),
], 0.95)

# The signature line, with a red heart (as in the app's About page).
from PIL import Image, ImageFont
img = Image.open(os.path.join(out, "card_end.png")).convert("RGBA")
d = ImageDraw.Draw(img)
f = mp.F("Inter-Regular.otf", 28)
hf = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28)
a, h, b = "Made with ", "\u2764", " from Greece by pixelrise"
wa, wh, wb = d.textlength(a, font=f), d.textlength(h, font=hf), d.textlength(b, font=f)
x = W // 2 - (wa + wh + wb) / 2
y = 730
d.text((x, y), a, font=f, fill=mp.DIM)
d.text((x + wa, y + 1), h, font=hf, fill=(224, 74, 90))
d.text((x + wa + wh, y), b, font=f, fill=mp.DIM)
img.convert("RGB").save(os.path.join(out, "card_end.png"))
