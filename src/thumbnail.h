// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Small HDR thumbnails for the folder grid.
#pragma once

#include <cstdint>
#include <vector>
#include "hdr_image.h"
#include "tonemap.h"

// A thumbnail kept in RGBE form (4 bytes per pixel, the Radiance encoding):
// small like an 8-bit image, but it still holds the HDR range, so exposure
// and tonemap changes can be applied to the grid without reloading files.
struct Thumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgbe;
    bool empty() const { return width <= 0 || height <= 0 || rgbe.empty(); }
};

// Size of an image of w x h when fitted inside boxW x boxH (aspect kept,
// never enlarged).
void ThumbFitSize(int w, int h, int boxW, int boxH, int& outW, int& outH);

// Shrinks `img` (averaging in linear light) to fit inside boxW x boxH.
Thumbnail MakeThumbnail(const HdrImage& img, int boxW, int boxH);

// Brightest luminance and the range in stops from the median to it (the same
// measure as Fix HDRI's report). The median comes from a sample of pixels.
void ImageQuickStats(const HdrImage& img, float& peakLum, float& rangeStops);

// Writes thumb.width * thumb.height BGRA pixels.
void TonemapThumbnail(const Thumbnail& thumb, const TonemapSettings& s, uint8_t* outBGRA);

// Exact-size area resample (used for the last step after halving).
HdrImage ResizeAreaHdr(const HdrImage& src, int dstW, int dstH);
