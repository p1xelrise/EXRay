// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// "Fix HDRI": finds the usual problems that make an HDRI light a 3D scene
// badly, and repairs what can be repaired.
//
//  - Clipped sun: the camera capped the sun at one value, so a flat plateau
//    sits where a tiny, extremely bright disc should be. The real values are
//    gone; the fix paints a sun disc of the real angular size (0.53 deg) with
//    a brightness estimated from the sky around it (clear-sky ratio of direct
//    sun to sky light for the sun's height), lowered for hazy skies.
//  - Broken pixels: NaN, infinite and negative values (black / white
//    artefacts and fireflies in renderers).
//  - Hot pixels: single pixels far brighter than everything around them.
//  - Not real HDR: nothing above 1.0, i.e. an ordinary picture saved in an
//    HDR format. This can only be reported, not fixed.
#pragma once

#include <cstddef>
#include <string>

#include "hdr_image.h"

struct HdriReport {
    int width = 0, height = 0;
    bool panorama = false;            // 2:1 equirectangular: angles are meaningful

    // Broken values (pixels with at least one such channel).
    size_t nanPixels = 0, infPixels = 0, negativePixels = 0;
    size_t hotPixels = 0;

    // Range.
    float maxValue = 0.0f;            // brightest finite channel value
    float maxLum = 0.0f;              // brightest finite luminance
    int maxX = 0, maxY = 0;           // ...and where it is
    float medianLum = 0.0f;
    float rangeStops = 0.0f;          // log2(brightest / median)
    bool notHdr = false;              // nothing above 1.0

    // Clipping: flat plateaus at the brightest value.
    bool clipped = false;
    int clippedAreas = 0;
    float clipLevel = 0.0f;           // luminance of the plateau (largest area)
    size_t clippedPixels = 0;         // pixels in the largest area

    // The sun: the largest clipped area above the horizon (panoramas only).
    enum class Sun {
        None,           // no clipped area
        Rebuildable,    // a clipped sun that can be rebuilt
        LooksOk,        // flat top, but already about as bright as a real sun
        TooLarge,       // clipped area too big to be a sun (window, overexposed sky)
        BelowHorizon,   // clipped areas only below the horizon (reflections, lamps)
        NotPanorama     // not 2:1, so no directions to work with
    };
    Sun sun = Sun::None;
    float sunX = 0.0f, sunY = 0.0f;   // image position of its centre
    float sunElevation = 0.0f;        // degrees above the horizon
    float sunSizeDeg = 0.0f;          // diameter of the clipped area, degrees
    float sunColor[3] = {1, 1, 1};    // colour of the glow around it (luminance 1)
    bool glowNeutral = true;          // that glow is almost white
    bool wideGlow = false;            // clipped area much wider than a sun (sunset or haze glow):
                                      // rebuilding a sharp sun there would add far too much light
    double clippedEnergy = 0.0;       // light the clipped area gives now (facing it)
    float lightShare = 1.0f;          // clippedEnergy / sunIrradiance: how much of the real sun is left
    double skyIrradiance = 0.0;       // light from the sky (without the sun) on a flat surface
    double sunIrradiance = 0.0;       // estimated direct sun, at 0 EV
    float sunPeak = 0.0f;             // luminance of the rebuilt disc, at 0 EV

    bool AnythingToFix() const {
        return nanPixels || infPixels || negativePixels || hotPixels || sun == Sun::Rebuildable;
    }
};

// Colour of the rebuilt sun.
enum class SunColorMode {
    Glow,       // the colour of the glow around the clipped area
    Natural     // daylight colour for the sun's height: warm near the horizon, nearly white high up
};

constexpr float kRealSunSizeDeg = 0.53f;
constexpr float kWideGlowDeg = 2.0f;  // clipped areas wider than this are a glow, not a sun

// How clear the sky was. A clipped file no longer shows how strong the sun
// was; haze takes light from the sun and gives it to the sky, so the same
// sky light means a weaker sun. Tested against real unclipped HDRIs: the
// right choice brings the sun light within about 25 % of the original.
enum class SkyType { Clear, LightHaze, Hazy };
float SkyEV(SkyType sky);             // 0, -1, -1.5 EV on the clear-sky estimate

struct HdriFixOptions {
    bool repairBroken = true;         // NaN / infinite / negative values
    bool removeHotPixels = true;
    bool rebuildSun = true;
    SkyType sky = SkyType::Clear;
    float sunStrengthEV = 0.0f;       // on top of the clear-sky estimate (SkyEV(sky))
    float sunSizeDeg = kRealSunSizeDeg;   // disc diameter; bigger = softer shadows, same total light
    SunColorMode sunColor = SunColorMode::Glow;
};

// Daylight colour (linear sRGB, luminance 1) of the sun at this height above the horizon.
void NaturalSunColor(float elevationDeg, float out[3]);

struct HdriFixResult {
    size_t repaired = 0;              // pixels with broken values fixed
    size_t hotRemoved = 0;
    bool sunRebuilt = false;
    float sunPeak = 0.0f;             // luminance written into the disc centre
};

// Default choices for a report: every fix that applies, except hot-pixel
// removal when there are so many that they're probably stars or lights, and
// a wide clipped glow. Clear sky, real sun size, colour from the glow.
HdriFixOptions DefaultFixOptions(const HdriReport& report);

// Analyses `img` (linear RGB). Uses all CPU cores.
HdriReport AnalyzeHdri(const HdrImage& img);

// Lots of "hot pixels" usually means stars or small lights, not faults.
bool HdriManyHotPixels(const HdriReport& r);

// Luminance the rebuilt sun disc gets at the given strength.
float SunPeakAt(const HdriReport& r, float strengthEV, float sizeDeg = kRealSunSizeDeg);

// Writes a fixed copy of `src` into `out`. `src` is not changed.
bool FixHdri(const HdrImage& src, const HdriReport& report, const HdriFixOptions& opts,
             HdrImage& out, HdriFixResult& result, std::string& error);
