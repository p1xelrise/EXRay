// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "resample.h"

#include <algorithm>
#include <new>
#include "parallel.h"

HdrImage HalveHdr(const HdrImage& src) {
    HdrImage dst;
    if (src.empty()) return dst;
    const int dw = HalfSize(src.width), dh = HalfSize(src.height);
    float* out = static_cast<float*>(std::malloc(static_cast<size_t>(dw) * dh * 3 * sizeof(float)));
    if (!out) throw std::bad_alloc();
    const float* in = src.rgb();
    const size_t sw = static_cast<size_t>(src.width);

    ParallelRows(dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* r0 = in + static_cast<size_t>(std::min(src.height - 1, y * 2)) * sw * 3;
            const float* r1 = in + static_cast<size_t>(std::min(src.height - 1, y * 2 + 1)) * sw * 3;
            float* d = out + static_cast<size_t>(y) * dw * 3;
            for (int x = 0; x < dw; ++x) {
                const size_t a = static_cast<size_t>(std::min(src.width - 1, x * 2)) * 3;
                const size_t b = static_cast<size_t>(std::min(src.width - 1, x * 2 + 1)) * 3;
                for (int c = 0; c < 3; ++c) {
                    // Keep NaN/Inf from spreading: treat them like the tonemapper does.
                    float v0 = r0[a + c], v1 = r0[b + c], v2 = r1[a + c], v3 = r1[b + c];
                    float sum = 0.0f;
                    for (float v : {v0, v1, v2, v3}) {
                        if (v != v) continue;                 // NaN -> 0
                        if (v > 65504.0f) v = 65504.0f;       // +Inf -> very bright
                        if (v < 0.0f) v = 0.0f;
                        sum += v;
                    }
                    d[x * 3 + c] = sum * 0.25f;
                }
            }
        }
    }, dw * 4);

    dst.pixels.reset(out);
    dst.width = dw;
    dst.height = dh;
    dst.format = src.format;
    dst.displayReferred = src.displayReferred;
    return dst;
}

HdrImage MakePreviewHdr(const HdrImage& src, int maxW, int maxH) {
    if (src.empty() || (src.width <= maxW && src.height <= maxH)) return HdrImage();
    HdrImage cur = HalveHdr(src);
    while (cur.width > maxW || cur.height > maxH) cur = HalveHdr(cur);
    return cur;
}

void HalveBGRA8(const uint8_t* src, int w, int h, uint8_t* dst) {
    const int dw = HalfSize(w), dh = HalfSize(h);
    ParallelRows(dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* r0 = src + static_cast<size_t>(std::min(h - 1, y * 2)) * w * 4;
            const uint8_t* r1 = src + static_cast<size_t>(std::min(h - 1, y * 2 + 1)) * w * 4;
            uint8_t* d = dst + static_cast<size_t>(y) * dw * 4;
            for (int x = 0; x < dw; ++x) {
                const int a = std::min(w - 1, x * 2) * 4, b = std::min(w - 1, x * 2 + 1) * 4;
                for (int c = 0; c < 4; ++c)
                    d[x * 4 + c] = static_cast<uint8_t>((r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c] + 2) >> 2);
            }
        }
    }, dw);
}
