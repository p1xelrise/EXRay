// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// 360° view: renders a perspective view out of an equirectangular (2:1) image.
#pragma once

#include <cstdint>

struct PanoView {
    double yaw = 0.0;     // degrees, 0 = the centre of the image, positive = turn right
    double pitch = 0.0;   // degrees, positive = look up (limited to +-89.9)
    double fov = 90.0;    // horizontal field of view in degrees
};

constexpr double kPanoMinFov = 20.0;
constexpr double kPanoMaxFov = 140.0;

// One copy of the (tonemapped) panorama: packed BGRA rows.
struct PanoSource {
    const uint8_t* bgra = nullptr;
    int width = 0, height = 0;
};

// True for images shaped like an equirectangular panorama (about 2:1).
inline bool LooksPanoramic(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const double a = static_cast<double>(w) / h;
    return a > 1.9 && a < 2.1;
}

// Renders the view into `dst` (dstW x dstH BGRA, rows packed). `levels` are
// the same panorama at decreasing sizes (largest first); the smallest one that
// still has enough detail for the field of view is used. Multi-threaded.
void RenderPano(const PanoSource* levels, int levelCount, const PanoView& view,
                uint8_t* dst, int dstW, int dstH);

// Screen point (sx, sy) in a viewW x viewH view -> pixel of the full-size
// panorama (imgW x imgH). Always succeeds (every direction hits the sphere).
void PanoScreenToImage(const PanoView& view, int viewW, int viewH, double sx, double sy,
                       int imgW, int imgH, int& x, int& y);

// Keeps yaw in -180..180, pitch and fov in range.
void NormalizePanoView(PanoView& v);
