// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Ground-truth test for Fix HDRI on real photographs.
//
// Each HDRI here has a real, unclipped sun. For each one:
//   1. The original must NOT be reported as a clipped sun.
//   2. A copy is clipped the way a camera would (every value capped at the
//      peak / 100, / 40 and / 10).
//   3. The clipped copy is analysed and fixed with the default options and
//      with the sky type that matches the photo.
//   4. The sun light within 2 degrees is compared with the original.
//
// Pass rules:
//   - right sky type: sun light within 70 % .. 135 % of the original
//   - wide glow (hazy / sunset sun): not ticked by default, and the default fix
//     adds no sun at all (so it can't over-light the scene)
//   - every case: the fix gives more light than the clipped copy had, unless
//     the default leaves the sun alone
//
//   ground_truth_test file1.hdr:clear file2.hdr:hazy ...   (clear | lighthaze | hazy | glow)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hdri_fix.h"
#include "image_io.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

double Lum(const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

// Light (luminance x solid angle) within `deg` of image position cx, cy.
double LightNear(const HdrImage& im, double cx, double cy, double deg) {
    const double W = im.width, H = im.height;
    auto dir = [&](double x, double y, double* d) {
        const double lat = kPi * (0.5 - y / H), lon = 2 * kPi * x / W - kPi;
        d[0] = std::cos(lat) * std::sin(lon); d[1] = std::sin(lat); d[2] = std::cos(lat) * std::cos(lon);
    };
    double c[3]; dir(cx, cy, c);
    const double cosMax = std::cos(deg * kPi / 180), r = deg / 180.0 * H + 3;
    double e = 0;
    for (int y = std::max(0, int(cy - r)); y < std::min(im.height, int(cy + r) + 1); ++y) {
        const double lat = kPi * (0.5 - (y + 0.5) / H);
        const double rx = r / std::max(0.05, std::cos(lat));
        for (int xx = int(cx - rx); xx <= int(cx + rx); ++xx) {
            const int x = ((xx % im.width) + im.width) % im.width;
            double d[3]; dir(x + 0.5, y + 0.5, d);
            if (d[0] * c[0] + d[1] * c[1] + d[2] * c[2] < cosMax) continue;
            const double om = (2 * kPi / W) * (kPi / H) * std::cos(lat);
            e += Lum(im.rgb() + (size_t(y) * im.width + x) * 3) * om;
        }
    }
    return e;
}

void CopyClipped(const HdrImage& src, float level, HdrImage& out) {
    out.width = src.width; out.height = src.height; out.format = src.format;
    out.pixels.reset(static_cast<float*>(std::malloc(src.valueCount() * sizeof(float))));
    const float* s = src.rgb();
    float* d = out.pixels.get();
    for (size_t i = 0; i < src.valueCount(); ++i) d[i] = std::min(s[i], level);
}

}  // namespace

int main(int argc, char** argv) {
    int failures = 0, cases = 0;
    auto fail = [&](const char* what) { std::printf("    FAIL: %s\n", what); ++failures; };
    for (int a = 1; a < argc; ++a) {
        std::string arg = argv[a];
        const size_t colon = arg.rfind(':');
        const std::string path = arg.substr(0, colon), kind = arg.substr(colon + 1);
        const bool glow = kind == "glow";
        const SkyType sky = kind == "hazy" ? SkyType::Hazy : kind == "lighthaze" ? SkyType::LightHaze : SkyType::Clear;
        HdrImage truth; std::string err;
        if (!LoadHdrFile(path, truth, err)) { std::printf("%s: %s\n", path.c_str(), err.c_str()); return 1; }
        const char* name = std::strrchr(path.c_str(), '/') ? std::strrchr(path.c_str(), '/') + 1 : path.c_str();

        const HdriReport orig = AnalyzeHdri(truth);
        std::printf("%s  (%s)  peak %.0f\n", name, kind.c_str(), orig.maxLum);
        ++cases;
        if (orig.sun == HdriReport::Sun::Rebuildable) fail("original reported as a clipped sun");
        else std::printf("    original: not clipped  OK\n");

        for (float div : {100.0f, 40.0f, 10.0f}) {
            const float level = orig.maxLum / div;
            HdrImage clipped;
            CopyClipped(truth, level, clipped);
            const HdriReport r = AnalyzeHdri(clipped);
            ++cases;
            std::printf("    clip at %-8.0f (peak/%3.0f): ", level, div);
            if (r.sun != HdriReport::Sun::Rebuildable) {
                // A huge clipped area or none at all: nothing is rebuilt, so nothing can be over-lit.
                std::printf("sun %d, size %.1f deg, not rebuilt  OK (left alone)\n", static_cast<int>(r.sun), r.sunSizeDeg);
                continue;
            }
            const double eT = LightNear(truth, r.sunX, r.sunY, 2), eC = LightNear(clipped, r.sunX, r.sunY, 2);
            HdriFixOptions def = DefaultFixOptions(r);
            HdrImage fixedDef; HdriFixResult res;
            if (!FixHdri(clipped, r, def, fixedDef, res, err)) { fail(err.c_str()); continue; }
            const double eD = LightNear(fixedDef, r.sunX, r.sunY, 2);
            std::printf("size %.2f deg%s  clipped keeps %3.0f%%  default (Clear) %4.0f%%",
                        r.sunSizeDeg, r.wideGlow ? " (wide glow)" : "", 100 * eC / eT, 100 * eD / eT);
            if (glow) {
                std::printf("\n");
                if (!r.wideGlow) fail("glow not recognised as a wide glow");
                if (def.rebuildSun) fail("wide glow ticked by default");
                if (std::fabs(eD - eC) > 1e-6 * eT + 1e-9) fail("default fix changed the glow");
                continue;
            }
            HdriFixOptions o = def;
            o.rebuildSun = true;
            o.sky = sky;
            HdrImage fixedSky;
            if (!FixHdri(clipped, r, o, fixedSky, res, err)) { fail(err.c_str()); continue; }
            const double eS = LightNear(fixedSky, r.sunX, r.sunY, 2);
            std::printf("  right sky %4.0f%%  peak %.0f (real %.0f)\n", 100 * eS / eT, res.sunPeak, orig.maxLum);
            if (eS < 0.70 * eT || eS > 1.35 * eT) fail("sun light with the right sky outside 70 .. 135 %");
            if (eS <= eC) fail("fix gave no more light than the clipped copy");
            if (r.wideGlow) fail("a real sun called a wide glow");
        }
    }
    std::printf(failures ? "\nFAILED (%d of %d checks)\n" : "\nALL OK (%d cases)\n", failures ? failures : cases, cases);
    return failures ? 1 : 0;
}
