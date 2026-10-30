# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Generates res/app.ico: a small HDRI-style sky with a bright sun.
# Run: python3 tools/make_icon.py
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # draw large, then shrink for smooth edges
out_dir = os.path.join(os.path.dirname(__file__), "..", "res")
os.makedirs(out_dir, exist_ok=True)

y = np.linspace(0.0, 1.0, S)[:, None]
horizon = 0.62

# Sky: deep blue at the top to warm orange at the horizon; dark ground below.
top = np.array([22, 40, 92], float)
mid = np.array([86, 110, 170], float)
low = np.array([246, 150, 72], float)
ground = np.array([28, 30, 38], float)

t = np.clip(y / horizon, 0, 1)
sky = np.where(t < 0.6,
               top + (mid - top) * (t / 0.6),
               mid + (low - mid) * ((t - 0.6) / 0.4))
img = np.where(y < horizon, sky, ground)
img = np.broadcast_to(img[:, None, :], (S, S, 3)).copy()   # (S,3) -> (S,S,3)

# Sun glow (additive), centered just above the horizon.
yy, xx = np.mgrid[0:S, 0:S] / S
cx, cy = 0.5, horizon - 0.10
d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
glow = np.exp(-(d / 0.16) ** 2) * 160 + np.exp(-(d / 0.05) ** 2) * 255
glow = np.where(yy < horizon, glow, glow * 0.25)  # reflection on the ground
img = img + glow[..., None] * np.array([1.0, 0.85, 0.6])
img = np.clip(img, 0, 255).astype(np.uint8)

base = Image.fromarray(img, "RGB").convert("RGBA")

# Solid sun disk.
draw = ImageDraw.Draw(base)
r = 0.085 * S
draw.ellipse([cx * S - r, cy * S - r, cx * S + r, cy * S + r], fill=(255, 250, 236, 255))

# Rounded-square mask.
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([int(S * 0.03), int(S * 0.03), int(S * 0.97), int(S * 0.97)],
                                       radius=int(S * 0.2), fill=255)
base.putalpha(mask)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
icon = base.resize((256, 256), Image.LANCZOS)
icon.save(os.path.join(out_dir, "app.ico"), sizes=[(s, s) for s in sizes])
icon.save(os.path.join(out_dir, "app_icon_preview.png"))
print("wrote", os.path.join(out_dir, "app.ico"))
