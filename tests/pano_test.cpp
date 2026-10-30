// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Renders several 360° views of a panorama to PNGs and times them.
// Usage: pano_test <in.exr|hdr> <out_dir>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "image_io.h"
#include "pano.h"
#include "resample.h"
#include "tonemap.h"
#include "stb_image_write.h"
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HdrImage img; std::string err;
    if (!LoadHdrFile(argv[1], img, err)) { std::printf("%s\n", err.c_str()); return 1; }
    // Tonemapped pyramid like the viewer's.
    std::vector<std::vector<uint8_t>> lv; std::vector<PanoSource> src;
    lv.emplace_back(); TonemapToBGRA8(img, TonemapSettings{0, Tonemap::ACES}, lv.back());
    int w = img.width, h = img.height; std::vector<std::pair<int,int>> sz{{w,h}};
    while (w > 256 && h > 128) { std::vector<uint8_t> n((size_t)HalfSize(w)*HalfSize(h)*4); HalveBGRA8(lv.back().data(), w, h, n.data()); w = HalfSize(w); h = HalfSize(h); lv.push_back(std::move(n)); sz.push_back({w,h}); }
    for (size_t i = 0; i < lv.size(); ++i) src.push_back({lv[i].data(), sz[i].first, sz[i].second});
    struct V { double yaw, pitch, fov; const char* n; } views[] = {{0,0,90,"front"},{90,0,90,"right"},{180,0,90,"back"},{0,60,100,"up"},{-45,-30,60,"downleft"},{0,0,140,"wide"}};
    const int W = 1600, H = 900;
    std::vector<uint8_t> out((size_t)W*H*4), rgb((size_t)W*H*3);
    for (auto& v : views) {
        PanoView pv{v.yaw, v.pitch, v.fov};
        auto t = std::chrono::steady_clock::now();
        RenderPano(src.data(), (int)src.size(), pv, out.data(), W, H);
        double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count() * 1000;
        int cx, cy; PanoScreenToImage(pv, W, H, W / 2.0, H / 2.0, img.width, img.height, cx, cy);
        std::printf("%-9s %.1f ms  centre -> image (%d,%d)\n", v.n, ms, cx, cy);
        for (size_t i = 0; i < (size_t)W*H; ++i) { rgb[i*3] = out[i*4+2]; rgb[i*3+1] = out[i*4+1]; rgb[i*3+2] = out[i*4]; }
        stbi_write_png((std::string(argv[2]) + "/" + v.n + ".png").c_str(), W, H, 3, rgb.data(), W * 3);
    }
    return 0;
}
