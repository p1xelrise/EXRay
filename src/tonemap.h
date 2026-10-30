// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Converts linear HDR pixels into displayable 8-bit sRGB.
#pragma once

#include <cstdint>
#include <vector>
#include "hdr_image.h"

enum class Tonemap {
    Clip,       // exposure + clamp, no curve
    Reinhard,   // simple x / (1 + x)
    ACES        // ACES filmic fit (Stephen Hill) -- the default
};

struct TonemapSettings {
    float exposureEV = 0.0f;           // 0 = default, +1 doubles brightness
    Tonemap curve = Tonemap::ACES;
};

// Writes width*height*4 bytes in BGRA order (what Windows bitmaps use),
// alpha set to 255, rows packed with no padding. Uses all CPU cores.
void TonemapToBGRA8(const HdrImage& img, const TonemapSettings& s, uint8_t* outBGRA);
void TonemapToBGRA8(const HdrImage& img, const TonemapSettings& s, std::vector<uint8_t>& outBGRA);

inline bool operator==(const TonemapSettings& a, const TonemapSettings& b) {
    return a.exposureEV == b.exposureEV && a.curve == b.curve;
}
inline bool operator!=(const TonemapSettings& a, const TonemapSettings& b) { return !(a == b); }
