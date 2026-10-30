// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Basic in-memory HDR image: linear RGB floats, row-major, top row first.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>

struct HdrImage {
    struct FreeDeleter { void operator()(float* p) const { std::free(p); } };

    int width = 0;
    int height = 0;
    // width * height * 3 floats, linear light. Allocated with malloc so the
    // decoders' own buffers can be adopted without an extra copy (big HDRIs
    // are several GB as floats).
    std::unique_ptr<float, FreeDeleter> pixels;
    std::string format;       // "EXR" or "HDR" (Radiance)
    // True for views that are already display colours in 0..1 (Cryptomatte
    // ID colours and masks): shown as they are, without exposure or tonemap.
    bool displayReferred = false;

    const float* rgb() const { return pixels.get(); }
    size_t valueCount() const { return static_cast<size_t>(width) * static_cast<size_t>(height) * 3; }
    bool empty() const { return width <= 0 || height <= 0 || !pixels; }
    void clear() { width = height = 0; pixels.reset(); format.clear(); displayReferred = false; }
};
