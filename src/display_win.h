// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows bitmaps for showing tonemapped images.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "hdr_image.h"
#include "tonemap.h"

// A 32-bit top-down bitmap whose pixels we write directly (a DIB section),
// so there is only one copy of the pixels in memory.
class Bitmap {
public:
    static std::unique_ptr<Bitmap> Create(int w, int h);   // throws std::bad_alloc on failure
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    int width() const { return w_; }
    int height() const { return h_; }
    uint8_t* bits() const { return bits_; }
    HBITMAP handle() const { return bmp_; }

private:
    Bitmap() = default;
    int w_ = 0, h_ = 0;
    HBITMAP bmp_ = nullptr;
    uint8_t* bits_ = nullptr;
};

using Pyramid = std::vector<std::unique_ptr<Bitmap>>;   // [0] = full size, then halves

// Tonemaps `img` with `s` into a single bitmap of the same size.
std::unique_ptr<Bitmap> TonemapToBitmap(const HdrImage& img, const TonemapSettings& s);

// Tonemaps `img` and builds shrunk copies down to roughly thumbnail size, so
// zoomed-out views draw quickly and smoothly.
Pyramid BuildPyramid(const HdrImage& img, const TonemapSettings& s);
