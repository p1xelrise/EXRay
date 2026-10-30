# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# Promo images for the Gumroad page: 1920x1080 covers and a 600x600 thumbnail.
# Run: python3 tools/make_promo.py   (screenshots in promo/shot_*.png)
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(__file__), "..")
P = os.path.join(ROOT, "promo")
FD = "/usr/share/fonts/opentype/inter/"
def F(name, size): return ImageFont.truetype(FD + name, size)

W, H = 1920, 1080
TEXT = (238, 238, 238)
DIM = (160, 164, 172)
ACCENT = (74, 139, 216)
WARM = (246, 166, 90)

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

def background(warm_x=0.75, warm_y=0.15):
    yy, xx = np.mgrid[0:H, 0:W].astype(float)
    img = np.zeros((H, W, 3)) + np.array([16, 17, 20])
    # soft blue light upper-left, warm sun glow on one side
    d1 = np.sqrt(((xx - W * 0.1) / W) ** 2 + ((yy - H * 0.0) / H) ** 2)
    d2 = np.sqrt(((xx - W * warm_x) / W) ** 2 + ((yy - H * warm_y) / H) ** 2)
    img += np.exp(-(d1 / 0.45) ** 2)[..., None] * np.array([18, 34, 70])
    img += np.exp(-(d2 / 0.35) ** 2)[..., None] * np.array([60, 34, 12])
    noise = np.random.default_rng(1).normal(0, 1.6, (H, W, 1))   # avoid banding
    return Image.fromarray(np.clip(img + noise, 0, 255).astype(np.uint8)).convert("RGBA")

def framed(shot, width, radius=14, cut=None):
    """Screenshot scaled to `width`, rounded corners, thin border, soft shadow. Returns (image, pad).
    `cut` = (y0, y1): rows of the screenshot to leave out."""
    s = Image.open(shot).convert("RGBA")
    if cut:
        y0, y1 = cut
        t = Image.new("RGBA", (s.width, s.height - (y1 - y0)))
        t.paste(s.crop((0, 0, s.width, y0)), (0, 0))
        t.paste(s.crop((0, y1, s.width, s.height)), (0, y0))
        s = t
    h = round(s.height * width / s.width)
    s = s.resize((width, h), Image.LANCZOS)
    m = Image.new("L", s.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, width - 1, h - 1], radius=radius, fill=255)
    s.putalpha(m)
    pad = 70
    out = Image.new("RGBA", (width + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    sh = Image.new("L", out.size, 0)
    ImageDraw.Draw(sh).rounded_rectangle([pad, pad + 18, pad + width, pad + h + 18], radius=radius, fill=200)
    sh = sh.filter(ImageFilter.GaussianBlur(28))
    out.paste(Image.new("RGBA", out.size, (0, 0, 0, 255)), (0, 0), sh)
    out.alpha_composite(s, (pad, pad))
    ImageDraw.Draw(out).rounded_rectangle([pad, pad, pad + width - 1, pad + h - 1], radius=radius,
                                          outline=(255, 255, 255, 40), width=2)
    return out, pad

def wrap(draw, text, font, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=font) <= width: cur = t
        else: lines.append(cur); cur = w
    lines.append(cur)
    return lines

def pro_badge(img, x, y, size=20):
    d = ImageDraw.Draw(img)
    f = F("Inter-Bold.otf", size)
    w = d.textlength("PRO", font=f)
    d.rounded_rectangle([x, y, x + w + size, y + size * 1.55], radius=int(size * 0.4), fill=WARM)
    d.text((x + size / 2, y + size * 0.775), "PRO", font=f, fill=(24, 20, 16), anchor="lm")

def text_block(img, x, y, width, kicker, title, body, chips=(), pro=False):
    d = ImageDraw.Draw(img)
    kf = F("Inter-SemiBold.otf", 22)
    d.text((x, y), kicker.upper(), font=kf, fill=ACCENT)
    if pro: pro_badge(img, x + d.textlength(kicker.upper(), font=kf) + 14, y - 2, 18)
    y += 50
    tf = F("InterDisplay-Bold.otf", 68)
    for line in wrap(d, title, tf, width):
        d.text((x, y), line, font=tf, fill=TEXT); y += 80
    y += 18
    bf = F("Inter-Regular.otf", 28)
    for line in wrap(d, body, bf, width):
        d.text((x, y), line, font=bf, fill=DIM); y += 42
    y += 34
    cf = F("Inter-Medium.otf", 22)
    cx = x
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(ov)
    spots = []
    for c in chips:
        tw = d.textlength(c, font=cf)
        if cx + tw + 36 > x + width: cx = x; y += 56
        od.rounded_rectangle([cx, y, cx + tw + 36, y + 42], radius=21, fill=(255, 255, 255, 18), outline=(255, 255, 255, 60), width=2)
        spots.append((cx + 18, y + 9, c))
        cx += tw + 48
    img.alpha_composite(ov)
    d = ImageDraw.Draw(img)
    for sx, sy, c in spots: d.text((sx, sy), c, font=cf, fill=TEXT)
    return y

def brand(img, x, y, size=44):
    img.alpha_composite(icon(size), (x, y))
    ImageDraw.Draw(img).text((x + size + 16, y + size / 2), "EXRay", font=F("Inter-SemiBold.otf", 24),
                             fill=TEXT, anchor="lm")

def save(img, name):
    img.convert("RGB").save(os.path.join(P, name), quality=95)
    print("wrote", name)

# 1. Hero: name on top, the image view below.
img = background(0.5, 0.05)
d = ImageDraw.Draw(img)
ic = icon(96)
title_font = F("InterDisplay-Bold.otf", 104)
tw = d.textlength("EXRay", font=title_font)
x0 = (W - (96 + 28 + tw)) // 2
img.alpha_composite(ic, (int(x0), 78))
d.text((x0 + 96 + 28, 126), "EXRay", font=title_font, fill=TEXT, anchor="lm")
d.text((W // 2, 214), "The fast HDR & EXR viewer for 3D artists", font=F("Inter-SemiBold.otf", 36), fill=TEXT, anchor="mt")
d.text((W // 2, 272), "Browse, inspect and fix your HDRIs and EXR renders — no Photoshop needed.", font=F("Inter-Regular.otf", 28),
       fill=DIM, anchor="mt")
d.text((W // 2, 324), "One portable .exe for Windows 11   ·   nothing to install   ·   works offline",
       font=F("Inter-Medium.otf", 24), fill=WARM, anchor="mt")
fr, pad = framed(os.path.join(P, "shot_image.png"), 1240)
img.alpha_composite(fr, (W // 2 - fr.width // 2, 392 - pad))
save(img, "01_hero.jpg")

# 2. Grid.
img = background(0.85, 0.2)
fr, pad = framed(os.path.join(P, "shot_grid.png"), 1140)
img.alpha_composite(fr, (W - fr.width + 20, (H - fr.height) // 2))
brand(img, 110, 90)
text_block(img, 110, 300, 560, "Thumbnail grid", "Find the right HDRI in seconds",
           "Open a folder and every .hdr and .exr shows up as a thumbnail — even 16K and bigger. No more opening them one by one.",
           ["Folders of any size", "Adjustable size", "Arrow-key browsing"])
save(img, "02_grid.jpg")

# 3. 360 view (image left, text right).
img = background(0.15, 0.25)
fr, pad = framed(os.path.join(P, "shot_360.png"), 1140)
img.alpha_composite(fr, (-20, (H - fr.height) // 2))
brand(img, 1250, 90)
text_block(img, 1250, 300, 580, "360° view", "Stand inside your HDRI",
           "Drag to look around and zoom the field of view. Check the horizon, the sun and the backplate before it goes into your scene.",
           ["Drag to look", "Wheel = field of view", "Exposure & tonemaps"])
save(img, "03_360.jpg")

# 4. Fix HDRI.
img = background(0.8, 0.1)
fr, pad = framed(os.path.join(P, "shot_fix.png"), 1140)
img.alpha_composite(fr, (W - fr.width + 20, (H - fr.height) // 2))
brand(img, 110, 90)
text_block(img, 110, 300, 560, "Fix HDRI", "Check it before it lights your scene",
           "One click finds a clipped sun, broken pixels and hot pixels. Rebuild the sun, repair the rest, and save a clean EXR or HDR.",
           ["Clipped sun rebuild", "NaN / hot pixels", "Before / after"])
save(img, "04_fix.jpg")

# 5. Render passes (image left, text right).
img = background(0.2, 0.9)
fr, pad = framed(os.path.join(P, "shot_passes.png"), 1140)
img.alpha_composite(fr, (-20, (H - fr.height) // 2))
brand(img, 1250, 90)
text_block(img, 1250, 300, 580, "EXR layers", "Look inside your renders",
           "Multi-layer EXRs from Unreal's Movie Render Queue, Blender and more: depth, normals, motion vectors, AO — shown so you can read them.",
           ["Multi-part & multi-view", "Readable data passes", "Real pixel values"])
save(img, "05_passes.jpg")

# 6. Cryptomatte: the ID colours and an isolated mask, overlapping.
img = background(0.85, 0.85)
fa, pa = framed(os.path.join(P, "shot_crypto.png"), 900)
fb, pb = framed(os.path.join(P, "shot_mask.png"), 900)
img.alpha_composite(fa, (W - fa.width - 250, 120 - pa))
img.alpha_composite(fb, (W - fb.width + 30, 480 - pb))
brand(img, 110, 90)
text_block(img, 110, 300, 560, "Cryptomatte", "Every object, one click away",
           "See object and material names under the mouse and isolate any of them as a clean mask. Ctrl+click to add more.",
           ["Names from the manifest", "Click to isolate", "Export the mask"])
save(img, "06_cryptomatte.jpg")

# 7. Features overview.
img = background(0.5, 1.1)
d = ImageDraw.Draw(img)
brand(img, 110, 80)
d.text((110, 170), "Everything for HDR & EXR files, in one small app", font=F("InterDisplay-Bold.otf", 58), fill=TEXT)
d.text((W - 110, 1052), "Free for everyone \u2014 every feature, no time limit", font=F("Inter-Medium.otf", 22), fill=DIM, anchor="rs")
tiles = [
    ("Thumbnail grid", "Whole folders of HDRIs at a glance, 16K and up."),
    ("Exposure & tonemaps", "−10 to +10 EV with Clip, Reinhard and ACES."),
    ("360° view", "Look around inside equirectangular HDRIs."),
    ("EXR layers", "Render passes, depth, normals, motion vectors."),
    ("Cryptomatte", "Object names under the mouse, one-click masks."),
    ("Pixel readout", "The real stored values, copied with Ctrl+C."),
    ("Fix HDRI", "Rebuild a clipped sun, repair broken pixels."),
    ("Export", "PNG / JPEG, single images or whole selections."),
    ("Portable", "One .exe, no install, no account, offline."),
]
tw, th, gx, gy = 540, 214, 40, 28
x0, y0 = 110, 278
for i, tile in enumerate(tiles):
    t, b = tile[0], tile[1]
    x = x0 + (i % 3) * (tw + gx); y = y0 + (i // 3) * (th + gy)
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle([x, y, x + tw, y + th], radius=18, fill=(255, 255, 255, 14), outline=(255, 255, 255, 40), width=2)
    img.alpha_composite(ov)
    d = ImageDraw.Draw(img)
    d.ellipse([x + 32, y + 36, x + 46, y + 50], fill=WARM if i == 6 else ACCENT)
    d.text((x + 62, y + 28), t, font=F("Inter-SemiBold.otf", 32), fill=TEXT)
    if len(tile) > 2: pro_badge(img, x + tw - 86, y + 30, 20); d = ImageDraw.Draw(img)
    bf = F("Inter-Regular.otf", 25)
    for k, line in enumerate(wrap(d, b, bf, tw - 64)):
        d.text((x + 32, y + 92 + k * 36), line, font=bf, fill=DIM)
save(img, "07_features.jpg")

# 8. Square thumbnail (600x600; made at 1200 for sharpness).
S = 1200
yy, xx = np.mgrid[0:S, 0:S].astype(float) / S
bg = np.zeros((S, S, 3)) + np.array([16, 17, 20])
bg += np.exp(-(np.sqrt((xx - .5) ** 2 + (yy - .35) ** 2) / .45) ** 2)[..., None] * np.array([40, 46, 80])
bg += np.exp(-(np.sqrt((xx - .5) ** 2 + (yy - 1.0) ** 2) / .4) ** 2)[..., None] * np.array([50, 28, 10])
th_img = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8)).convert("RGBA")
th_img.alpha_composite(icon(600), (S // 2 - 300, 130))
d = ImageDraw.Draw(th_img)
d.text((S // 2, 790), "EXRay", font=F("InterDisplay-Bold.otf", 190), fill=TEXT, anchor="mt")
d.text((S // 2, 1010), "HDR & EXR viewer", font=F("InterDisplay-Medium.otf", 72), fill=DIM, anchor="mt")
th_img.resize((600, 600), Image.LANCZOS).convert("RGB").save(os.path.join(P, "08_thumbnail_600.png"))
print("wrote 08_thumbnail_600.png")
