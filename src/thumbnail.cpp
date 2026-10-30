// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "thumbnail.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <new>
#include "resample.h"

namespace {

inline float Clean(float v) {
    if (v != v) return 0.0f;              // NaN
    if (v > 65504.0f) return 65504.0f;    // +Inf / huge
    return v < 0.0f ? 0.0f : v;
}

void EncodeRGBE(float r, float g, float b, uint8_t* out) {
    float v = std::max({r, g, b});
    if (v < 1e-32f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e = 0;
    float m = std::frexp(v, &e);           // v = m * 2^e, m in [0.5, 1)
    float scale = m * 256.0f / v;
    out[0] = static_cast<uint8_t>(std::min(255.0f, r * scale));
    out[1] = static_cast<uint8_t>(std::min(255.0f, g * scale));
    out[2] = static_cast<uint8_t>(std::min(255.0f, b * scale));
    out[3] = static_cast<uint8_t>(e + 128);
}

// For each destination index: the source indices it covers and their weights.
struct Taps { std::vector<int> start, count; std::vector<float> weights; };

Taps AreaTaps(int srcN, int dstN) {
    Taps t;
    const double ratio = static_cast<double>(srcN) / dstN;
    for (int d = 0; d < dstN; ++d) {
        const double a = d * ratio, b = (d + 1) * ratio;
        const int s0 = static_cast<int>(std::floor(a));
        const int s1 = std::min(srcN, static_cast<int>(std::ceil(b)));
        t.start.push_back(s0);
        t.count.push_back(s1 - s0);
        double total = 0;
        std::vector<double> w;
        for (int s = s0; s < s1; ++s) {
            double ww = std::min<double>(b, s + 1) - std::max<double>(a, s);
            w.push_back(std::max(0.0, ww));
            total += w.back();
        }
        for (double ww : w) t.weights.push_back(static_cast<float>(total > 0 ? ww / total : 0));
    }
    return t;
}

}  // namespace

void ThumbFitSize(int w, int h, int boxW, int boxH, int& outW, int& outH) {
    if (w <= 0 || h <= 0) { outW = outH = 0; return; }
    double s = std::min({static_cast<double>(boxW) / w, static_cast<double>(boxH) / h, 1.0});
    outW = std::max(1, static_cast<int>(std::lround(w * s)));
    outH = std::max(1, static_cast<int>(std::lround(h * s)));
}

HdrImage ResizeAreaHdr(const HdrImage& src, int dstW, int dstH) {
    HdrImage dst;
    if (src.empty() || dstW <= 0 || dstH <= 0) return dst;
    const Taps tx = AreaTaps(src.width, dstW), ty = AreaTaps(src.height, dstH);

    // Horizontal pass into a temporary (src.height x dstW), then vertical.
    std::vector<float> tmp(static_cast<size_t>(src.height) * dstW * 3);
    for (int y = 0; y < src.height; ++y) {
        const float* row = src.rgb() + static_cast<size_t>(y) * src.width * 3;
        float* out = tmp.data() + static_cast<size_t>(y) * dstW * 3;
        size_t wi = 0;
        for (int x = 0; x < dstW; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = 0; k < tx.count[x]; ++k) {
                const float w = tx.weights[wi + k];
                const float* p = row + static_cast<size_t>(tx.start[x] + k) * 3;
                for (int c = 0; c < 3; ++c) acc[c] += Clean(p[c]) * w;
            }
            wi += tx.count[x];
            for (int c = 0; c < 3; ++c) out[x * 3 + c] = acc[c];
        }
    }

    float* px = static_cast<float*>(std::malloc(static_cast<size_t>(dstW) * dstH * 3 * sizeof(float)));
    if (!px) throw std::bad_alloc();
    size_t wi = 0;
    for (int y = 0; y < dstH; ++y) {
        float* out = px + static_cast<size_t>(y) * dstW * 3;
        std::fill(out, out + static_cast<size_t>(dstW) * 3, 0.0f);
        for (int k = 0; k < ty.count[y]; ++k) {
            const float w = ty.weights[wi + k];
            const float* in = tmp.data() + static_cast<size_t>(ty.start[y] + k) * dstW * 3;
            for (int i = 0; i < dstW * 3; ++i) out[i] += in[i] * w;
        }
        wi += ty.count[y];
    }
    dst.pixels.reset(px);
    dst.width = dstW;
    dst.height = dstH;
    dst.format = src.format;
    dst.displayReferred = src.displayReferred;
    return dst;
}

Thumbnail MakeThumbnail(const HdrImage& img, int boxW, int boxH) {
    Thumbnail t;
    if (img.empty()) return t;
    int tw = 0, th = 0;
    ThumbFitSize(img.width, img.height, boxW, boxH, tw, th);

    // Halve (cheap, multi-threaded) while still at least twice the target,
    // then one exact area resample.
    const HdrImage* cur = &img;
    HdrImage halved;
    while (cur->width >= tw * 2 && cur->height >= th * 2) {
        halved = HalveHdr(*cur);
        cur = &halved;
    }
    HdrImage small;
    if (cur->width != tw || cur->height != th) {
        small = ResizeAreaHdr(*cur, tw, th);
        cur = &small;
    }

    t.width = tw;
    t.height = th;
    t.rgbe.resize(static_cast<size_t>(tw) * th * 4);
    const float* p = cur->rgb();
    for (size_t i = 0, n = static_cast<size_t>(tw) * th; i < n; ++i)
        EncodeRGBE(Clean(p[i * 3]), Clean(p[i * 3 + 1]), Clean(p[i * 3 + 2]), &t.rgbe[i * 4]);
    return t;
}

void TonemapThumbnail(const Thumbnail& thumb, const TonemapSettings& s, uint8_t* outBGRA) {
    if (thumb.empty()) return;
    const size_t n = static_cast<size_t>(thumb.width) * thumb.height;
    HdrImage tmp;
    float* px = static_cast<float*>(std::malloc(n * 3 * sizeof(float)));
    if (!px) throw std::bad_alloc();
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* q = &thumb.rgbe[i * 4];
        if (q[3] == 0) { px[i * 3] = px[i * 3 + 1] = px[i * 3 + 2] = 0.0f; continue; }
        const float f = std::ldexp(1.0f, static_cast<int>(q[3]) - (128 + 8));
        px[i * 3 + 0] = (q[0] + 0.5f) * f;
        px[i * 3 + 1] = (q[1] + 0.5f) * f;
        px[i * 3 + 2] = (q[2] + 0.5f) * f;
    }
    tmp.pixels.reset(px);
    tmp.width = thumb.width;
    tmp.height = thumb.height;
    TonemapToBGRA8(tmp, s, outBGRA);
}

void ImageQuickStats(const HdrImage& img, float& peakLum, float& rangeStops) {
    peakLum = 0.0f;
    rangeStops = 0.0f;
    if (img.empty()) return;
    const size_t n = static_cast<size_t>(img.width) * img.height;
    const float* px = img.rgb();
    const size_t step = std::max<size_t>(1, n / 262144);   // about 262,000 samples for the median
    std::vector<float> sample;
    sample.reserve(n / step + 1);
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float* p = px + i * 3;
        const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (!(l >= 0.0f) || l > 3.0e38f) continue;   // NaN, negative, infinite
        if (l > peak) peak = l;
        if (i % step == 0) sample.push_back(l);
    }
    peakLum = peak;
    if (sample.empty() || peak <= 0.0f) return;
    std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(sample.size() / 2), sample.end());
    const float median = sample[sample.size() / 2];
    const float base = std::max({median, peak * 1e-9f, 1e-9f});
    rangeStops = static_cast<float>(std::log2(peak / base));
}
