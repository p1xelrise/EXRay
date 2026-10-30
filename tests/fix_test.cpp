// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Native test for the HDRI fix: a synthetic sky with a known sun, clipped like
// a camera would, plus broken and hot pixels. Checks the analysis, the sun's
// rebuilt energy against the unclipped original, and EXR/HDR saving.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "export.h"
#include "hdri_fix.h"
#include "image_io.h"

static const double kPi = 3.14159265358979323846, kDeg = kPi / 180.0;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static HdrImage Make(int w, int h) {
    HdrImage img;
    img.width = w; img.height = h; img.format = "HDR";
    img.pixels.reset(static_cast<float*>(std::calloc(static_cast<size_t>(w) * h * 3, sizeof(float))));
    return img;
}

static void Dir(int w, int h, double x, double y, double d[3]) {
    const double la = kPi * (0.5 - y / h), lo = 2 * kPi * x / w - kPi;
    d[0] = std::cos(la) * std::sin(lo); d[1] = std::sin(la); d[2] = std::cos(la) * std::cos(lo);
}

static double Omega(int w, int h, int y) { return (2 * kPi / w) * (kPi / h) * std::cos(kPi * (0.5 - (y + 0.5) / h)); }

// Light from within `radiusDeg` of direction `c` (luminance x solid angle).
static double EnergyNear(const HdrImage& img, const double c[3], double radiusDeg) {
    double e = 0, cl = std::cos(radiusDeg * kDeg);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            double d[3]; Dir(img.width, img.height, x + 0.5, y + 0.5, d);
            if (d[0] * c[0] + d[1] * c[1] + d[2] * c[2] < cl) continue;
            const float* p = img.rgb() + (static_cast<size_t>(y) * img.width + x) * 3;
            e += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * Omega(img.width, img.height, y);
        }
    return e;
}

static void SavePreview(const HdrImage& img, int cx, int cy, int r, const char* name) {
    // A tonemapped crop around the sun as a PPM, for looking at by eye.
    FILE* f = std::fopen(name, "wb");
    std::fprintf(f, "P6 %d %d 255\n", 2 * r, 2 * r);
    for (int y = cy - r; y < cy + r; ++y)
        for (int x = cx - r; x < cx + r; ++x) {
            const float* p = img.rgb() + (static_cast<size_t>(std::clamp(y, 0, img.height - 1)) * img.width + ((x % img.width) + img.width) % img.width) * 3;
            for (int c = 0; c < 3; ++c) {
                float v = std::isfinite(p[c]) ? std::max(0.0f, p[c]) * 0.15f : 1.0f;
                v = v / (1 + v);
                std::fputc(static_cast<int>(std::pow(v, 1 / 2.2f) * 255), f);
            }
        }
    std::fclose(f);
}

static void RunCase(int w, double elevDeg, double lonDeg, float clipAt, bool expectFix = true) {
    const int h = w / 2;
    HdrImage truth = Make(w, h);
    double sun[3];
    {
        const double la = elevDeg * kDeg, lo = lonDeg * kDeg;
        sun[0] = std::cos(la) * std::sin(lo); sun[1] = std::sin(la); sun[2] = std::cos(la) * std::cos(lo);
    }
    // Sky: brighter towards the horizon-ish, blue; ground grey; an aureole around the sun.
    double skyE = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double d[3]; Dir(w, h, x + 0.5, y + 0.5, d);
            float* p = truth.pixels.get() + (static_cast<size_t>(y) * w + x) * 3;
            const double ang = std::acos(std::clamp(d[0] * sun[0] + d[1] * sun[1] + d[2] * sun[2], -1.0, 1.0)) / kDeg;
            if (d[1] > 0) {
                const double base = 0.8 + 0.6 * (1 - d[1]);
                const double glow = 6.0 * std::exp(-ang / 1.5) + 1.5 * std::exp(-ang / 15.0);
                p[0] = static_cast<float>(0.55 * base + glow); p[1] = static_cast<float>(0.75 * base + glow); p[2] = static_cast<float>(1.1 * base + glow * 0.95);
                skyE += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * d[1] * Omega(w, h, y);
            } else {
                p[0] = p[1] = p[2] = 0.25f;
            }
        }
    // The sun disc (supersampled), direct light = clear-sky ratio x sky light.
    const double ratio = 20.0 * (1.0 - std::exp(-std::max(1.0, elevDeg) / 50.0));   // same clear-sky curve as hdri_fix.cpp
    const double dni = ratio * skyE;
    const double r = 0.2665 * kDeg, L = dni / (kPi * r * r);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double d[3]; Dir(w, h, x + 0.5, y + 0.5, d);
            if (d[0] * sun[0] + d[1] * sun[1] + d[2] * sun[2] < std::cos(2 * kDeg)) continue;
            int in = 0;
            for (int sy = 0; sy < 8; ++sy) for (int sx = 0; sx < 8; ++sx) {
                Dir(w, h, x + (sx + 0.5) / 8, y + (sy + 0.5) / 8, d);
                if (d[0] * sun[0] + d[1] * sun[1] + d[2] * sun[2] >= std::cos(r)) ++in;
            }
            float* p = truth.pixels.get() + (static_cast<size_t>(y) * w + x) * 3;
            const float c = in / 64.0f;
            for (int k = 0; k < 3; ++k) p[k] = p[k] * (1 - c) + static_cast<float>(L) * c;
        }

    // The "camera" version: clipped, plus broken and hot pixels.
    HdrImage cam = Make(w, h);
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n * 3; ++i) cam.pixels.get()[i] = std::min(truth.rgb()[i], clipAt);
    float* cp = cam.pixels.get();
    const size_t ground = (static_cast<size_t>(h) * 3 / 4) * w + w / 3;
    cp[ground * 3 + 1] = NAN;
    cp[(ground + 10) * 3 + 0] = INFINITY;
    cp[(ground + 20) * 3 + 2] = -0.5f;
    cp[(ground + 7 * w) * 3 + 0] = cp[(ground + 7 * w) * 3 + 1] = cp[(ground + 7 * w) * 3 + 2] = 400.0f;   // hot pixel

    HdriReport rep = AnalyzeHdri(cam);
    if (!expectFix) {   // unclipped: the sun must be left alone
        std::printf("\n== unclipped %dx%d: clipped %d sun %d (level %.0f, estimate %.0f)\n", w, h, rep.clipped,
                    static_cast<int>(rep.sun), rep.clipLevel, rep.sunPeak);
        CHECK(rep.sun != HdriReport::Sun::Rebuildable, "unclipped sun marked for rebuilding");
        return;
    }
    std::printf("\n== %dx%d, sun at %.0f deg elevation, clipped at %.0f\n", w, h, elevDeg, clipAt);
    std::printf("  NaN %zu  inf %zu  neg %zu  hot %zu  max %.1f  median %.3f  range %.1f stops\n",
                rep.nanPixels, rep.infPixels, rep.negativePixels, rep.hotPixels, rep.maxValue, rep.medianLum, rep.rangeStops);
    std::printf("  clipped %d (%d areas, %zu px at %.2f)  sun %d at (%.1f, %.1f) elev %.2f size %.2f deg  colour %.2f %.2f %.2f\n",
                rep.clipped, rep.clippedAreas, rep.clippedPixels, rep.clipLevel, static_cast<int>(rep.sun), rep.sunX, rep.sunY,
                rep.sunElevation, rep.sunSizeDeg, rep.sunColor[0], rep.sunColor[1], rep.sunColor[2]);
    std::printf("  sky E %.4f (true %.4f)  sun E %.3f (true %.3f)  peak %.0f (true %.0f)\n",
                rep.skyIrradiance, skyE, rep.sunIrradiance, dni, rep.sunPeak, L);

    CHECK(rep.nanPixels == 1 && rep.infPixels == 1 && rep.negativePixels == 1, "broken pixel counts");
    CHECK(rep.hotPixels == 1, "hot pixel count %zu", rep.hotPixels);
    CHECK(rep.clipped && rep.sun == HdriReport::Sun::Rebuildable, "sun not found as rebuildable");
    CHECK(std::fabs(rep.sunElevation - elevDeg) < 0.3, "elevation %.2f", rep.sunElevation);
    CHECK(std::fabs(rep.sunIrradiance / dni - 1) < 0.15, "sun energy off: %.3f vs %.3f", rep.sunIrradiance, dni);

    HdrImage fixed;
    HdriFixResult res;
    std::string err;
    CHECK(FixHdri(cam, rep, HdriFixOptions{}, fixed, res, err), "fix failed: %s", err.c_str());
    std::printf("  fixed: repaired %zu, hot removed %zu, sun %d peak %.0f\n", res.repaired, res.hotRemoved, res.sunRebuilt, res.sunPeak);
    CHECK(res.repaired == 3 && res.hotRemoved == 1 && res.sunRebuilt, "fix result counts");

    // Sun light within 1 deg: truth vs clipped vs fixed.
    const double eT = EnergyNear(truth, sun, 1.0), eC = EnergyNear(cam, sun, 1.0), eF = EnergyNear(fixed, sun, 1.0);
    std::printf("  light within 1 deg: true %.3f  clipped %.3f (%.1f%%)  fixed %.3f (%.1f%%)\n",
                eT, eC, 100 * eC / eT, eF, 100 * eF / eT);
    CHECK(std::fabs(eF / eT - 1) < 0.15, "fixed sun energy %.1f%% of truth", 100 * eF / eT);
    std::printf("  clipped sun holds %.2f%% of the real light (report %.2f%%)\n", 100 * eC / eT, 100 * rep.lightShare);
    CHECK(rep.lightShare < 0.05f, "light share %.3f", rep.lightShare);

    // A bigger, softer sun with the natural colour: same total light.
    {
        HdriFixOptions o; o.sunSizeDeg = 2.0f; o.sunColor = SunColorMode::Natural;
        HdrImage soft; HdriFixResult r2;
        CHECK(FixHdri(cam, rep, o, soft, r2, err), "fix (2 deg) failed");
        const double eS = EnergyNear(soft, sun, 2.5);
        float col[3]; NaturalSunColor(rep.sunElevation, col);
        std::printf("  2 deg sun: peak %.0f (vs %.0f), light within 2.5 deg %.1f%% of truth, natural colour %.2f %.2f %.2f\n",
                    r2.sunPeak, res.sunPeak, 100 * eS / EnergyNear(truth, sun, 2.5), col[0], col[1], col[2]);
        CHECK(std::fabs(eS / EnergyNear(truth, sun, 2.5) - 1) < 0.15, "2 deg sun energy");
        CHECK(r2.sunPeak < res.sunPeak / 10, "2 deg sun should be much dimmer per pixel");
    }
    for (size_t i = 0; i < n * 3; ++i)
        if (!std::isfinite(fixed.rgb()[i]) || fixed.rgb()[i] < 0) { CHECK(false, "broken value left at %zu", i / 3); break; }

    char name[64];
    std::snprintf(name, sizeof name, "build/fix_%d_before.ppm", w);
    SavePreview(cam, static_cast<int>(rep.sunX), static_cast<int>(rep.sunY), 24, name);
    std::snprintf(name, sizeof name, "build/fix_%d_after.ppm", w);
    SavePreview(fixed, static_cast<int>(rep.sunX), static_cast<int>(rep.sunY), 24, name);

    // Save and read back in all three formats.
    const struct { HdrSaveFormat f; const char* path; float tol; } outs[] = {
        {HdrSaveFormat::ExrFloat, "build/fix_out_f.exr", 0.0f},
        {HdrSaveFormat::ExrHalf, "build/fix_out_h.exr", 0.002f},
        {HdrSaveFormat::Radiance, "build/fix_out.hdr", 0.01f},
    };
    for (const auto& o : outs) {
        CHECK(SaveHdrImage(fixed, o.path, o.f, err), "save %s: %s", o.path, err.c_str());
        HdrImage back;
        CHECK(LoadHdrFile(o.path, back, err), "reload %s: %s", o.path, err.c_str());
        if (back.empty()) continue;
        double worst = 0;
        float peakBack = 0;
        for (size_t i = 0; i < n * 3; ++i) {
            const float a = std::min(fixed.rgb()[i], o.f == HdrSaveFormat::ExrHalf ? kHalfMax : 1e30f), b = back.rgb()[i];
            peakBack = std::max(peakBack, b);
            // error relative to the pixel's brightest channel (RGBE shares one exponent)
            const float* q = fixed.rgb() + (i / 3) * 3;
            const float ref = std::min(std::max({q[0], q[1], q[2]}), o.f == HdrSaveFormat::ExrHalf ? kHalfMax : 1e30f);
            if (ref > 1e-3f) worst = std::max(worst, static_cast<double>(std::fabs(b - a) / ref));
        }
        std::printf("  %s: worst relative error %.5f, brightest %.0f\n", o.path, worst, peakBack);
        CHECK(worst <= o.tol + 1e-6, "%s error %.5f", o.path, worst);
    }
}

int main() {
    RunCase(2048, 30.0, 40.0, 20.0f);
    RunCase(1024, 12.0, -150.0, 8.0f);
    RunCase(4096, 62.0, 170.0, 50.0f);
    RunCase(8192, 40.0, 10.0, 1e9f, false);   // nothing clipped (but the planted broken pixels)

    // An ordinary picture saved as HDR: everything within 0..1.
    {
        HdrImage ldr = Make(400, 200);
        for (size_t i = 0; i < 400 * 200 * 3; ++i) ldr.pixels.get()[i] = static_cast<float>((i * 7919) % 1000) / 999.0f;
        HdriReport r = AnalyzeHdri(ldr);
        std::printf("\n== LDR: notHdr %d  max %.3f  clipped %d  sun %d\n", r.notHdr, r.maxValue, r.clipped, static_cast<int>(r.sun));
        CHECK(r.notHdr && !r.clipped, "LDR not detected");
    }
    // A clean HDRI with an unclipped sun: nothing to fix.
    {
        const int w = 1024, h = 512;
        HdrImage img = Make(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float* p = img.pixels.get() + (static_cast<size_t>(y) * w + x) * 3;
                const float v = y < h / 2 ? 1.0f + 0.001f * x : 0.2f;
                p[0] = p[1] = p[2] = v;
            }
        // a smooth unclipped peak
        for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
            float* p = img.pixels.get() + (static_cast<size_t>(100 + dy) * w + 500 + dx) * 3;
            const float v = 5000.0f * std::exp(-(dx * dx + dy * dy) / 1.5f);
            p[0] += v; p[1] += v; p[2] += v;
        }
        HdriReport r = AnalyzeHdri(img);
        std::printf("== clean: clipped %d  hot %zu  anything %d\n", r.clipped, r.hotPixels, r.AnythingToFix());
        CHECK(!r.clipped && !r.AnythingToFix(), "clean image flagged");
    }
    // Natural sun colour: warm near the horizon, nearly white high up.
    {
        float lo[3], hi[3]; NaturalSunColor(3.0f, lo); NaturalSunColor(60.0f, hi);
        std::printf("== natural colour 3 deg: %.2f %.2f %.2f   60 deg: %.2f %.2f %.2f\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        CHECK(lo[0] > 1.4f && lo[2] < 0.5f, "low sun not warm");
        CHECK(hi[0] > 1.0f && hi[0] < 1.25f && hi[2] > 0.75f, "high sun not near white");
    }
    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
