// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Native (Linux) test for the shared decode + tonemap code.
// Usage: decode_test <input.exr|input.hdr> <output.png> [write_hdr_copy.hdr]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "image_io.h"
#include "tonemap.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s in out.png [copy.hdr]\n", argv[0]); return 2; }
    HdrImage img;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!LoadHdrFile(argv[1], img, err)) { std::printf("LOAD FAILED: %s\n", err.c_str()); return 1; }
    auto t1 = std::chrono::steady_clock::now();

    float mn = 1e30f, mx = -1e30f; double sum = 0;
    for (size_t i = 0; i < img.valueCount(); ++i) { float v = img.rgb()[i]; mn = std::min(mn, v); mx = std::max(mx, v); sum += v; }
    std::printf("%s: %s %dx%d  min %.4f  max %.4f  mean %.4f  load %.2fs\n", argv[1], img.format.c_str(),
                img.width, img.height, mn, mx, sum / img.valueCount(),
                std::chrono::duration<double>(t1 - t0).count());

    std::vector<uint8_t> bgra;
    TonemapToBGRA8(img, TonemapSettings{}, bgra);
    auto t2 = std::chrono::steady_clock::now();
    std::printf("tonemap %.3fs\n", std::chrono::duration<double>(t2 - t1).count());

    std::vector<uint8_t> rgb(static_cast<size_t>(img.width) * img.height * 3);
    for (size_t i = 0, n = rgb.size() / 3; i < n; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }
    stbi_write_png(argv[2], img.width, img.height, 3, rgb.data(), img.width * 3);
    if (argc > 3) stbi_write_hdr(argv[3], img.width, img.height, 3, img.rgb());
    return 0;
}
