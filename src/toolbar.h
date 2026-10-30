// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The dark toolbar along the top of the window (custom drawn).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "tonemap.h"

constexpr float kMinExposureEV = -10.0f;
constexpr float kMaxExposureEV = 10.0f;
constexpr float kSunCheckMinEV = -24.0f;   // the Sun check may go below the slider (a real sun is 100,000+)

enum class ToolId { None, Grid, Image, Slider, Reset, Clip, Reinhard, Aces, Fix, Pano, Fit, Actual, Prev, Next };

struct ToolbarState {
    float exposure = 0.0f;
    Tonemap curve = Tonemap::ACES;
    bool gridMode = false;    // the folder grid is showing
    bool canGrid = false;     // there is a folder to show
    bool canImage = false;    // there is an image to show / open
    bool hasImage = false;    // exposure & tonemap controls are usable
    bool viewControls = false;// Fit / 100% are usable (single image view)
    bool fit = true;          // "Fit" is the active zoom mode
    bool canPano = false;     // the image is a 2:1 panorama (360\u00b0 view possible)
    bool pano = false;        // the 360\u00b0 view is on
    bool canFix = false;      // a colour image is showing (Fix HDRI possible)
    bool fixOn = false;       // a fix is applied to the image on screen
    bool actualSize = false;  // zoom is exactly 100%
    bool canPrev = false;
    bool canNext = false;
    ToolId hover = ToolId::None;
    ToolId pressed = ToolId::None;
};

class Toolbar {
public:
    // Recomputes control positions for the given client width and DPI.
    void Layout(int clientWidth, UINT dpi);
    int Height() const { return height_; }

    void Draw(HDC dc, const ToolbarState& st, HFONT font) const;
    ToolId HitTest(POINT pt) const;

    // Exposure value for a mouse x position over the slider (snapped to 0.1 EV).
    float ExposureFromX(int x) const;

    // Smallest client width (in physical pixels) at which everything fits.
    static int MinClientWidth(UINT dpi);

private:
    int Scale(int v) const;
    int ExposureToX(float ev) const;

    UINT dpi_ = 96;
    int height_ = 44;
    RECT bar_{}, grid_{}, image_{}, modeGroup_{}, label_{}, slider_{}, value_{}, reset_{};
    RECT clip_{}, reinhard_{}, aces_{}, group_{};
    RECT fix_{}, pano_{}, fit_{}, actual_{}, prev_{}, next_{};
    int trackLeft_ = 0, trackRight_ = 0, trackY_ = 0;
};
