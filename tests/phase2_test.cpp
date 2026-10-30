// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Native test for phase 2's shared code: preview downscaling, exposure and
// tonemap curves, and 8-bit halving. Writes a contact sheet of the variants.
// Usage: phase2_test <input> <out_dir>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "image_io.h"
#include "resample.h"
#include "tonemap.h"
#include "stb_image_write.h"

static double Secs(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
}

static void SavePng(const std::string& path, const std::vector<uint8_t>& bgra, int w, int h) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string out = argv[2];
    HdrImage img; std::string err;
    auto t = std::chrono::steady_clock::now();
    if (!LoadHdrFile(argv[1], img, err)) { std::printf("load failed: %s\n", err.c_str()); return 1; }
    std::printf("loaded %dx%d in %.2fs\n", img.width, img.height, Secs(t));

    t = std::chrono::steady_clock::now();
    HdrImage prev = MakePreviewHdr(img, 2048, 2048);
    std::printf("preview %dx%d in %.3fs\n", prev.width, prev.height, Secs(t));
    const HdrImage& p = prev.empty() ? img : prev;

    // Per-slider-step cost: tonemap the preview.
    std::vector<uint8_t> buf;
    t = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) TonemapToBGRA8(p, TonemapSettings{0.1f * i, Tonemap::ACES}, buf);
    std::printf("preview tonemap: %.1f ms per step\n", Secs(t) * 100.0);

    // Variants for visual check.
    const Tonemap curves[] = {Tonemap::Clip, Tonemap::Reinhard, Tonemap::ACES};
    const char* names[] = {"clip", "reinhard", "aces"};
    for (int c = 0; c < 3; ++c)
        for (float ev : {-3.0f, 0.0f, 3.0f}) {
            TonemapToBGRA8(p, TonemapSettings{ev, curves[c]}, buf);
            char fn[256];
            std::snprintf(fn, sizeof fn, "%s/%s_%+.0f.png", out.c_str(), names[c], ev);
            SavePng(fn, buf, p.width, p.height);
        }

    // Full tonemap + 8-bit pyramid (what the background render does).
    t = std::chrono::steady_clock::now();
    std::vector<uint8_t> full;
    TonemapToBGRA8(img, TonemapSettings{}, full);
    int w = img.width, h = img.height, levels = 1;
    std::vector<uint8_t> cur = std::move(full), next;
    while (w > 256 && h > 128) {
        next.resize(static_cast<size_t>(HalfSize(w)) * HalfSize(h) * 4);
        HalveBGRA8(cur.data(), w, h, next.data());
        cur.swap(next); w = HalfSize(w); h = HalfSize(h); ++levels;
    }
    std::printf("full render + %d levels: %.2fs (smallest %dx%d)\n", levels, Secs(t), w, h);
    SavePng(out + "/pyramid_smallest.png", cur, w, h);
    return 0;
}
