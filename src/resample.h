// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Box-filter downscaling, used for quick previews and for the display pyramid.
#pragma once

#include <cstdint>
#include "hdr_image.h"

// Size of an image halved (never below 1 pixel).
inline int HalfSize(int v) { return v > 1 ? v / 2 : 1; }

// Halves a linear HDR image with a 2x2 box filter (averaging in linear light).
HdrImage HalveHdr(const HdrImage& src);

// Returns a copy shrunk by halving until it is no larger than maxW x maxH.
// Returns an empty image if `src` already fits (use `src` itself then).
HdrImage MakePreviewHdr(const HdrImage& src, int maxW, int maxH);

// Halves a packed BGRA 8-bit image (w x h) into dst (HalfSize(w) x HalfSize(h)).
void HalveBGRA8(const uint8_t* src, int w, int h, uint8_t* dst);
