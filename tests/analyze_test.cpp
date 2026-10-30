// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the Fix HDRI analysis (and the fix) on real files: no crashes, sensible reports.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include "hdri_fix.h"
#include "image_io.h"
int main(int argc, char** argv) {
    const char* sun[] = {"none", "REBUILD", "looks-ok", "too-large", "below-horizon", "not-pano"};
    for (int a = 1; a < argc; ++a) {
        HdrImage img; std::string err;
        if (!LoadHdrFile(argv[a], img, err)) { std::printf("%-46s load failed: %s\n", argv[a] + std::string(argv[a]).rfind('/') + 1, err.c_str()); continue; }
        auto t0 = std::chrono::steady_clock::now();
        HdriReport r = AnalyzeHdri(img);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        HdrImage out; HdriFixResult fr;
        bool ok = FixHdri(img, r, HdriFixOptions{}, out, fr, err);
        size_t bad = 0;
        for (size_t i = 0; ok && i < out.valueCount(); ++i) if (!std::isfinite(out.rgb()[i]) || out.rgb()[i] < 0) ++bad;
        std::printf("%-40s %5dx%-5d %.2fs max %-9.4g stops %4.1f notHdr %d | nan %zu inf %zu neg %zu hot %zu | clip %d areas %d lvl %.4g | sun %s | fixed: rep %zu hot %zu sun %d, left bad %zu\n",
            argv[a] + std::string(argv[a]).rfind('/') + 1, r.width, r.height, s, r.maxValue, r.rangeStops, r.notHdr,
            r.nanPixels, r.infPixels, r.negativePixels, r.hotPixels, r.clipped, r.clippedAreas, r.clipLevel, sun[(int)r.sun],
            fr.repaired, fr.hotRemoved, fr.sunRebuilt, bad);
    }
}
