# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
import numpy as np
from PIL import Image, ImageDraw

def icon(size):
    S = 1024
    y = np.linspace(0, 1, S)[:, None]; hz = 0.62
    top, mid, low, gnd = map(lambda c: np.array(c, float), [(22, 40, 92), (86, 110, 170), (246, 150, 72), (28, 30, 38)])
    t = np.clip(y / hz, 0, 1)
    sky = np.where(t < 0.6, top + (mid - top) * (t / 0.6), mid + (low - mid) * ((t - 0.6) / 0.4))
    img = np.broadcast_to(np.where(y < hz, sky, gnd)[:, None, :], (S, S, 3)).copy()
    yy, xx = np.mgrid[0:S, 0:S] / S
    cx, cy = 0.5, hz - 0.10
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    glow = np.exp(-(d / 0.16) ** 2) * 160 + np.exp(-(d / 0.05) ** 2) * 255
    glow = np.where(yy < hz, glow, glow * 0.25)
    img = np.clip(img + glow[..., None] * np.array([1.0, 0.85, 0.6]), 0, 255).astype(np.uint8)
    base = Image.fromarray(img).convert("RGBA")
    r = 0.085 * S
    ImageDraw.Draw(base).ellipse([cx * S - r, cy * S - r, cx * S + r, cy * S + r], fill=(255, 250, 236, 255))
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle([int(S * .03)] * 2 + [int(S * .97)] * 2, radius=int(S * .2), fill=255)
    base.putalpha(m)
    return base.resize((size, size), Image.LANCZOS)

