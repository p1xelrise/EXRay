// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Writes a single-part EXR with several named layers, like a render pass file:
// R,G,B,A (beauty), diffuse.R/G/B, depth.Z, N.X/Y/Z, mask.  Half and float mixed.
// Usage: make_layered <out.exr>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tinyexr.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int W = 640, H = 320;
    const char* names[] = {"A", "B", "G", "R", "N.X", "N.Y", "N.Z", "depth.Z", "diffuse.B", "diffuse.G", "diffuse.R", "mask"};
    const int nc = 12;
    std::vector<std::vector<float>> ch(nc, std::vector<float>(static_cast<size_t>(W) * H));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = static_cast<size_t>(y) * W + x;
            float u = x / float(W), v = y / float(H);
            float dx = u - 0.5f, dy = (v - 0.5f) * 0.5f;
            float d = std::sqrt(dx * dx + dy * dy);
            float sphere = d < 0.2f ? 1.0f : 0.0f;
            ch[3][i] = 4.0f * u * sphere + 0.1f;   // R  (bright, HDR)
            ch[2][i] = 2.0f * v * sphere + 0.1f;   // G
            ch[1][i] = 0.5f + 0.0f * sphere;        // B
            ch[0][i] = sphere;                      // A
            ch[4][i] = dx * 5; ch[5][i] = dy * 5; ch[6][i] = sphere; // N
            ch[7][i] = 10.0f * d;                   // depth
            ch[8][i] = 0.2f; ch[9][i] = 0.6f * sphere; ch[10][i] = 0.9f * u; // diffuse
            ch[11][i] = (x / 40 + y / 40) % 2 ? 1.0f : 0.0f;                  // mask
        }

    EXRHeader header; InitEXRHeader(&header);
    EXRImage image; InitEXRImage(&image);
    image.num_channels = nc;
    std::vector<float*> ptrs(nc);
    for (int c = 0; c < nc; ++c) ptrs[c] = ch[c].data();
    image.images = reinterpret_cast<unsigned char**>(ptrs.data());
    image.width = W; image.height = H;

    header.num_channels = nc;
    header.channels = static_cast<EXRChannelInfo*>(std::malloc(sizeof(EXRChannelInfo) * nc));
    header.pixel_types = static_cast<int*>(std::malloc(sizeof(int) * nc));
    header.requested_pixel_types = static_cast<int*>(std::malloc(sizeof(int) * nc));
    for (int c = 0; c < nc; ++c) {
        std::memset(&header.channels[c], 0, sizeof(EXRChannelInfo));
        std::strncpy(header.channels[c].name, names[c], 255);
        header.channels[c].x_sampling = header.channels[c].y_sampling = 1;
        header.pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        header.requested_pixel_types[c] = (c == 7) ? TINYEXR_PIXELTYPE_FLOAT : TINYEXR_PIXELTYPE_HALF;
    }
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
    const char* err = nullptr;
    int ret = SaveEXRImageToFile(&image, &header, argv[1], &err);
    std::printf("save %d %s\n", ret, err ? err : "");
    std::free(header.channels); std::free(header.pixel_types); std::free(header.requested_pixel_types);
    return ret;
}
