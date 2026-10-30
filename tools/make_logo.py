# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Converts res/logo_source.png into res/logo.bin, built into the .exe:
#   uint32 width, uint32 height, then width*height BGRA pixels,
#   alpha-premultiplied (ready for GDI's AlphaBlend).
# Run: python3 tools/make_logo.py
import os
import struct
from PIL import Image

here = os.path.join(os.path.dirname(__file__), "..", "res")
src = Image.open(os.path.join(here, "logo_source.png")).convert("RGBA")
bbox = src.getchannel("A").getbbox()          # trim empty borders
if bbox:
    src = src.crop(bbox)

H = 200                                       # stored size (enough for 200% scaling); shrunk at run time
W = round(src.width * H / src.height)
img = src.convert("RGBa").resize((W, H), Image.LANCZOS)   # resize premultiplied: no dark fringes
r, g, b, a = img.split()
data = Image.merge("RGBA", (b, g, r, a)).tobytes()        # BGRA order

with open(os.path.join(here, "logo.bin"), "wb") as f:
    f.write(struct.pack("<II", W, H))
    f.write(data)
print("logo.bin", W, "x", H)
