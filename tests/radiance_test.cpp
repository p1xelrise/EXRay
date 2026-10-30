// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Checks the own Radiance reader against stb_image, plus flat / flipped / huge files.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "export.h"
#include "image_io.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#include "stb_image.h"
static int fails = 0;
static bool Same(const char* path) {
    HdrImage a; std::string e;
    if (!LoadHdrFile(path, a, e)) { std::printf("%s: own reader failed: %s\n", path, e.c_str()); ++fails; return false; }
    int w, h, c; float* b = stbi_loadf(path, &w, &h, &c, 3);
    if (!b) { std::printf("%s: stb failed (%s), own ok %dx%d\n", path, stbi_failure_reason(), a.width, a.height); return true; }
    size_t diff = 0;
    for (size_t i = 0; i < (size_t)w * h * 3; ++i) if (a.rgb()[i] != b[i]) ++diff;
    std::printf("%s: %dx%d, %zu values differ from stb\n", path, w, h, diff);
    if (diff || w != a.width || h != a.height) ++fails;
    stbi_image_free(b);
    return true;
}
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) Same(argv[i]);
    // Flat scanlines (width >= 32768 can't use new-style RLE) and a +Y file.
    {
        HdrImage img; img.width = 32768; img.height = 16; img.format = "HDR";
        img.pixels.reset((float*)std::malloc(img.valueCount() * 4));
        for (size_t i = 0; i < img.valueCount(); ++i) img.pixels.get()[i] = (float)((i * 2654435761u) % 1000) / 37.0f;
        std::string e;
        SaveHdrImage(img, "build/flat32k.hdr", HdrSaveFormat::Radiance, e);
        Same("build/flat32k.hdr");
        // +Y: same data with the rows reversed and the header flag changed
        std::vector<unsigned char> d; FILE* f = std::fopen("build/fix_out.hdr", "rb");
        std::fseek(f, 0, SEEK_END); d.resize(std::ftell(f)); std::fseek(f, 0, SEEK_SET); std::fread(d.data(), 1, d.size(), f); std::fclose(f);
        std::string s(d.begin(), d.end());
        size_t p = s.find("-Y "); s[p] = '+';
        f = std::fopen("build/flipped.hdr", "wb"); std::fwrite(s.data(), 1, s.size(), f); std::fclose(f);
        Same("build/flipped.hdr");
    }
    // Reduced decode equals a box average.
    {
        std::vector<unsigned char> d; std::string e; ReadFileBytes("build/fix_out.hdr", d, e);
        HdrImage full, red; DecodeHdrImage(d, full, e); DecodeHdrImageReduced(d, red, e, 512);
        int k = full.width / red.width; double worst = 0;
        for (int y = 0; y < red.height; ++y) for (int x = 0; x < red.width; ++x) for (int c = 0; c < 3; ++c) {
            double s = 0; for (int dy = 0; dy < k; ++dy) for (int dx = 0; dx < k; ++dx) s += full.rgb()[((size_t)(y*k+dy)*full.width + x*k+dx)*3+c];
            s /= k*k; double v = red.rgb()[((size_t)y*red.width+x)*3+c];
            worst = std::max(worst, std::fabs(v - s) / std::max(1e-3, s));
        }
        std::printf("reduced %dx%d -> %dx%d (x%d), worst rel. error %.2e\n", full.width, full.height, red.width, red.height, k, worst);
        if (worst > 1e-4) ++fails;
    }
    // Huge: over stb's 2 GB limit (20000 x 10000 = 2.4 GB of floats).
    if (argc > 0 && std::getenv("HUGE")) {
        HdrImage img; img.width = 20000; img.height = 10000; img.format = "HDR";
        img.pixels.reset((float*)std::malloc(img.valueCount() * 4));
        for (int y = 0; y < img.height; ++y) for (int x = 0; x < img.width; ++x) {
            float* p = img.pixels.get() + ((size_t)y * img.width + x) * 3;
            p[0] = 1.0f + x * 1e-4f; p[1] = 0.5f + y * 1e-4f; p[2] = (x + y) % 7 * 0.25f;
        }
        std::string e;
        SaveHdrImage(img, "build/huge.hdr", HdrSaveFormat::Radiance, e);
        img.clear();
        auto t0 = std::chrono::steady_clock::now();
        HdrImage back;
        bool ok = LoadHdrFile("build/huge.hdr", back, e);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("huge 20000x10000: %s %dx%d in %.1fs; pixel (12345,6789) = %.4f %.4f %.4f (expect ~%.4f %.4f %.4f)\n",
                    ok ? "loaded" : e.c_str(), back.width, back.height, s,
                    back.rgb()[((size_t)6789 * 20000 + 12345) * 3], back.rgb()[((size_t)6789 * 20000 + 12345) * 3 + 1],
                    back.rgb()[((size_t)6789 * 20000 + 12345) * 3 + 2], 1.0f + 12345e-4f, 0.5f + 6789e-4f, (12345 + 6789) % 7 * 0.25f);
        if (!ok) ++fails;
        back.clear();
        std::vector<unsigned char> d; ReadFileBytes("build/huge.hdr", d, e);
        HdrImage red; t0 = std::chrono::steady_clock::now();
        ok = DecodeHdrImageReduced(d, red, e, 2048);
        s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("huge thumbnail decode: %dx%d in %.1fs (%.0f MB instead of %.0f MB)\n", red.width, red.height, s,
                    red.valueCount() * 4 / 1048576.0, 20000.0 * 10000 * 12 / 1048576.0);
        std::remove("build/huge.hdr");
    }
    std::printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
