// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tonemap.h"

#include <algorithm>
#include <cmath>
#include "parallel.h"

namespace {

constexpr int kLutSize = 4096;

// Linear [0,1] -> 8-bit sRGB lookup table, built once.
struct SrgbLut {
    uint8_t v[kLutSize + 1];
    SrgbLut() {
        for (int i = 0; i <= kLutSize; ++i) {
            double x = static_cast<double>(i) / kLutSize;
            double s = (x <= 0.0031308) ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
            v[i] = static_cast<uint8_t>(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
        }
    }
};
const SrgbLut& Lut() { static SrgbLut lut; return lut; }

inline uint8_t ToSrgb8(float x, const SrgbLut& lut) {
    if (!(x > 0.0f)) return 0;          // also catches NaN
    if (x >= 1.0f) return 255;
    return lut.v[static_cast<int>(x * kLutSize + 0.5f)];
}

inline float Sanitize(float x) {
    if (!std::isfinite(x)) return x > 0.0f ? 65504.0f : 0.0f;   // +inf -> very bright, NaN/-inf -> black
    return x < 0.0f ? 0.0f : x;
}

// ACES filmic fit by Stephen Hill (sRGB/Rec.709 input and output).
inline void AcesHill(float& r, float& g, float& b) {
    float a0 = 0.59719f * r + 0.35458f * g + 0.04823f * b;
    float a1 = 0.07600f * r + 0.90834f * g + 0.01566f * b;
    float a2 = 0.02840f * r + 0.13383f * g + 0.83777f * b;
    auto rrt = [](float v) {
        float a = v * (v + 0.0245786f) - 0.000090537f;
        float b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
        return a / b;
    };
    a0 = rrt(a0); a1 = rrt(a1); a2 = rrt(a2);
    r =  1.60475f * a0 - 0.53108f * a1 - 0.07367f * a2;
    g = -0.10208f * a0 + 1.10813f * a1 - 0.00605f * a2;
    b = -0.00327f * a0 - 0.07276f * a1 + 1.07602f * a2;
}

void TonemapRows(const HdrImage& img, const TonemapSettings& s, uint8_t* out, int y0, int y1) {
    const SrgbLut& lut = Lut();
    const float scale = std::exp2(s.exposureEV);
    const size_t w = static_cast<size_t>(img.width);
    for (int y = y0; y < y1; ++y) {
        const float* src = img.rgb() + static_cast<size_t>(y) * w * 3;
        uint8_t* dst = out + static_cast<size_t>(y) * w * 4;
        for (size_t x = 0; x < w; ++x) {
            if (img.displayReferred) {   // already display colours: just quantise
                auto q = [](float v) { return static_cast<uint8_t>(v > 0.0f ? (v < 1.0f ? v * 255.0f + 0.5f : 255.0f) : 0.0f); };
                dst[x * 4 + 0] = q(src[x * 3 + 2]);
                dst[x * 4 + 1] = q(src[x * 3 + 1]);
                dst[x * 4 + 2] = q(src[x * 3 + 0]);
                dst[x * 4 + 3] = 255;
                continue;
            }
            float r = Sanitize(src[x * 3 + 0]) * scale;
            float g = Sanitize(src[x * 3 + 1]) * scale;
            float b = Sanitize(src[x * 3 + 2]) * scale;
            switch (s.curve) {
                case Tonemap::Clip: break;
                case Tonemap::Reinhard: r = r / (1.0f + r); g = g / (1.0f + g); b = b / (1.0f + b); break;
                case Tonemap::ACES: AcesHill(r, g, b); break;
            }
            dst[x * 4 + 0] = ToSrgb8(b, lut);
            dst[x * 4 + 1] = ToSrgb8(g, lut);
            dst[x * 4 + 2] = ToSrgb8(r, lut);
            dst[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

void TonemapToBGRA8(const HdrImage& img, const TonemapSettings& s, uint8_t* out) {
    if (img.empty()) return;
    Lut();  // build before threads start
    ParallelRows(img.height, [&](int y0, int y1) { TonemapRows(img, s, out, y0, y1); }, img.width);
}

void TonemapToBGRA8(const HdrImage& img, const TonemapSettings& s, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4);
    TonemapToBGRA8(img, s, out.data());
}
