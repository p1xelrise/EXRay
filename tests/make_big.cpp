// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Makes large test HDRIs by upscaling a small one (nearest neighbour).
// Usage: make_big <input> <width> <out.exr> <out.hdr>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "image_io.h"
#include "tinyexr.h"
#include "stb_image_write.h"

int main(int argc, char** argv) {
    if (argc < 5) return 2;
    HdrImage src; std::string err;
    if (!LoadHdrFile(argv[1], src, err)) { std::printf("%s\n", err.c_str()); return 1; }
    int W = std::atoi(argv[2]), H = W / 2;
    std::vector<float> big(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) {
        int sy = y * src.height / H;
        for (int x = 0; x < W; ++x) {
            int sx = x * src.width / W;
            for (int c = 0; c < 3; ++c)
                big[(static_cast<size_t>(y) * W + x) * 3 + c] = src.rgb()[(static_cast<size_t>(sy) * src.width + sx) * 3 + c];
        }
    }
    const char* e = nullptr;
    int r = SaveEXR(big.data(), W, H, 3, /*save_as_fp16=*/1, argv[3], &e);
    std::printf("exr save %d %s\n", r, e ? e : "");
    std::printf("hdr save %d\n", stbi_write_hdr(argv[4], W, H, 3, big.data()));
    return 0;
}
