# Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
# SPDX-License-Identifier: GPL-3.0-or-later
# The app logo (Horizon Sun, v0.10) for the promo images and landing page.
# res/app_icon_preview.png is written by tools/make_icon.py.
import os
from PIL import Image

_SRC = os.path.join(os.path.dirname(__file__), "..", "res", "app_icon_preview.png")

def icon(size):
    return Image.open(_SRC).convert("RGBA").resize((size, size), Image.LANCZOS)
