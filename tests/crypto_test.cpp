// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Usage: crypto_test <file.exr> <out_dir>
// Lists layers, writes every layer PNG (crypto as ID view), prints the objects
// at some pixels, and writes an isolated mask of the object at the centre.
#include <cstdio>
#include <string>
#include <vector>
#include "exr_doc.h"
#include "image_io.h"
#include "tonemap.h"
#include "stb_image_write.h"

static void Save(const HdrImage& img, const std::string& fn) {
    std::vector<uint8_t> bgra, rgb(static_cast<size_t>(img.width) * img.height * 3);
    TonemapToBGRA8(img, TonemapSettings{}, bgra);
    for (size_t k = 0; k < rgb.size() / 3; ++k) { rgb[k*3] = bgra[k*4+2]; rgb[k*3+1] = bgra[k*4+1]; rgb[k*3+2] = bgra[k*4]; }
    stbi_write_png(fn.c_str(), img.width, img.height, 3, rgb.data(), img.width * 3);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<unsigned char> data; std::string err;
    if (!ReadFileBytes(argv[1], data, err)) { std::printf("read: %s\n", err.c_str()); return 1; }
    HdrImage img; std::shared_ptr<ExrDocument> doc;
    if (!DecodeHdrImage(data, img, err, &doc)) { std::printf("decode: %s\n", err.c_str()); return 1; }
    if (!doc) { std::printf("single layer\n"); return 0; }
    std::printf("cryptomattes: %d\n", doc->CryptoCount());
    for (int c = 0; c < doc->CryptoCount(); ++c)
        std::printf("  [%d] %s: %zu id/coverage pairs, %zu names\n", c, doc->Crypto(c).name.c_str(),
                    doc->Crypto(c).ranks.size(), doc->Crypto(c).names.size());
    for (int i = 0; i < (int)doc->Layers().size(); ++i) {
        const ExrLayer& L = doc->Layers()[i];
        std::printf("layer %d%s %s (%s)\n", i, i == doc->DefaultLayer() ? "*" : "", ExrDocument::DisplayName(L).c_str(), L.channels.c_str());
        HdrImage li;
        if (!doc->BuildLayer(i, li, err)) { std::printf("  build failed: %s\n", err.c_str()); continue; }
        Save(li, std::string(argv[2]) + "/layer" + std::to_string(i) + ".png");
        if (L.crypto < 0) continue;
        const int pts[][2] = {{960, 540}, {300, 300}, {1600, 800}, {100, 1000}, {960, 100}};
        for (auto& pt : pts) {
            auto hits = doc->CryptoAt(L.crypto, pt[0], pt[1]);
            std::printf("  (%d,%d):", pt[0], pt[1]);
            for (auto& h : hits) std::printf("  %s %.0f%%", doc->CryptoName(L.crypto, h.id).c_str(), h.coverage * 100);
            std::printf("\n");
        }
        auto hits = doc->CryptoAt(L.crypto, 960, 540);
        if (!hits.empty()) {
            HdrImage mask;
            doc->BuildCryptoView(L.crypto, {hits[0].id}, mask, err);
            Save(mask, std::string(argv[2]) + "/mask_center.png");
        }
    }
    return 0;
}
