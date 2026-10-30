// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Lists the layers of EXR files and writes each layer as a PNG (ACES, 0 EV).
// Usage: layers_test <out_dir> <files...>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "exr_doc.h"
#include "image_io.h"
#include "tonemap.h"
#include "stb_image_write.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string outDir = argv[1];
    int fileNo = 0;
    for (int a = 2; a < argc; ++a, ++fileNo) {
        std::vector<unsigned char> data;
        std::string err;
        if (!ReadFileBytes(argv[a], data, err)) { std::printf("%s: READ FAILED %s\n", argv[a], err.c_str()); continue; }
        auto t0 = std::chrono::steady_clock::now();
        HdrImage img;
        std::shared_ptr<ExrDocument> doc;
        bool ok = DecodeHdrImage(data, img, err, &doc);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) { std::printf("%s: FAILED %s\n", argv[a], err.c_str()); continue; }
        std::printf("%s: %dx%d %.2fs, %s\n", argv[a], img.width, img.height, s,
                    doc ? "layers:" : "single layer");
        if (!doc) continue;
        for (int i = 0; i < static_cast<int>(doc->Layers().size()); ++i) {
            const ExrLayer& L = doc->Layers()[static_cast<size_t>(i)];
            HdrImage li;
            if (!doc->BuildLayer(i, li, err)) { std::printf("   [%d] BUILD FAILED %s\n", i, err.c_str()); continue; }
            std::printf("   [%d]%s %-28s %-18s part %d  %dx%d\n", i, i == doc->DefaultLayer() ? "*" : " ",
                        ExrDocument::DisplayName(L).c_str(), (L.channels + " " + ExrDocument::ViewName(L.view)).c_str(), L.part, li.width, li.height);
            std::vector<uint8_t> bgra, rgb(static_cast<size_t>(li.width) * li.height * 3);
            TonemapToBGRA8(li, TonemapSettings{}, bgra);
            for (size_t k = 0; k < rgb.size() / 3; ++k) { rgb[k*3] = bgra[k*4+2]; rgb[k*3+1] = bgra[k*4+1]; rgb[k*3+2] = bgra[k*4]; }
            char fn[512];
            std::snprintf(fn, sizeof fn, "%s/f%02d_l%02d.png", outDir.c_str(), fileNo, i);
            stbi_write_png(fn, li.width, li.height, 3, rgb.data(), li.width * 3);
        }
    }
    return 0;
}
