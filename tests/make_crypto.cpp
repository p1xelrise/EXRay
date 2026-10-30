// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Writes a Blender-style Cryptomatte EXR without metadata (names come only
// from the channel names): R,G,B + CryptoMaterial00.R/G/B/A. Two objects with
// an anti-aliased diagonal edge.  Usage: make_crypto <out.exr>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "tinyexr.h"
static float IdFloat(unsigned bits) { float f; std::memcpy(&f, &bits, 4); return f; }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int W = 256, H = 128;
    const char* names[] = {"B", "CryptoMaterial00.A", "CryptoMaterial00.B", "CryptoMaterial00.G", "CryptoMaterial00.R", "G", "R"};
    const int nc = 7;
    std::vector<std::vector<float>> ch(nc, std::vector<float>(W * H));
    const float idA = IdFloat(0x3f12a3b4u), idB = IdFloat(0x4e00ff11u);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int i = y * W + x;
        float t = (x - y * 2) / 4.0f + 0.5f; t = t < 0 ? 0 : t > 1 ? 1 : t;   // coverage of A
        ch[6][i] = t; ch[5][i] = 0.2f; ch[0][i] = 1 - t;
        bool aFirst = t >= 0.5f;
        ch[4][i] = aFirst ? idA : idB; ch[3][i] = aFirst ? t : 1 - t;       // rank 0 id/cov
        ch[2][i] = aFirst ? idB : idA; ch[1][i] = aFirst ? 1 - t : t;       // rank 1 id/cov
    }
    EXRHeader h; InitEXRHeader(&h); EXRImage im; InitEXRImage(&im);
    std::vector<float*> ptr(nc); for (int c = 0; c < nc; ++c) ptr[c] = ch[c].data();
    im.images = reinterpret_cast<unsigned char**>(ptr.data()); im.num_channels = nc; im.width = W; im.height = H;
    h.num_channels = nc;
    h.channels = (EXRChannelInfo*)calloc(nc, sizeof(EXRChannelInfo));
    h.pixel_types = (int*)malloc(nc * sizeof(int)); h.requested_pixel_types = (int*)malloc(nc * sizeof(int));
    for (int c = 0; c < nc; ++c) { strncpy(h.channels[c].name, names[c], 255); h.channels[c].x_sampling = h.channels[c].y_sampling = 1;
        h.pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT; h.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT; }
    h.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
    const char* err = nullptr;
    int r = SaveEXRImageToFile(&im, &h, argv[1], &err);
    std::printf("save %d %s\n", r, err ? err : "");
    return r;
}
