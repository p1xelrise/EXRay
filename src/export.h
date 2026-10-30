// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Saving the tonemapped image as PNG or JPEG.
#pragma once

#include <string>
#include "hdr_image.h"
#include "image_io.h"
#include "tonemap.h"

enum class ExportFormat { Png, Jpeg };

// The format implied by a file name (.jpg / .jpeg -> JPEG, anything else PNG).
ExportFormat ExportFormatFromPath(const PathString& path);

// Tonemaps `img` with `s` at full resolution and writes it to `path`.
// Unicode paths work. Returns false and fills `error` on failure.
bool ExportImage(const HdrImage& img, const TonemapSettings& s, const PathString& path,
                 ExportFormat format, std::string& error);

// Saving the linear image itself (e.g. a fixed HDRI), not a tonemapped picture.
enum class HdrSaveFormat {
    ExrFloat,   // OpenEXR, 32-bit float, ZIP compressed: keeps every value
    ExrHalf,    // OpenEXR, 16-bit half float: half the size, values above 65,504 are capped
    Radiance    // Radiance .hdr (RGBE): opens everywhere, about 1% precision
};

// Largest value a 16-bit half float can hold.
constexpr float kHalfMax = 65504.0f;

// Writes `img` (linear RGB) to `path`. Unicode paths work. On failure the
// partly written file is removed and `error` says why.
bool SaveHdrImage(const HdrImage& img, const PathString& path, HdrSaveFormat format, std::string& error);
