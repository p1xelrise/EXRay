// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "toolbar.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace {

const COLORREF kBarBg      = RGB(0x25, 0x25, 0x25);
const COLORREF kBarBorder  = RGB(0x33, 0x33, 0x33);
const COLORREF kButton     = RGB(0x33, 0x33, 0x33);
const COLORREF kButtonHot  = RGB(0x40, 0x40, 0x40);
const COLORREF kButtonDown = RGB(0x4a, 0x4a, 0x4a);
const COLORREF kAccent     = RGB(0x3a, 0x7b, 0xc8);
const COLORREF kAccentHot  = RGB(0x4a, 0x8b, 0xd8);
const COLORREF kText       = RGB(0xdd, 0xdd, 0xdd);
const COLORREF kTextDim    = RGB(0x8a, 0x8a, 0x8a);
const COLORREF kTextOff    = RGB(0x5a, 0x5a, 0x5a);
const COLORREF kTrack      = RGB(0x4a, 0x4a, 0x4a);
const COLORREF kThumb      = RGB(0xe6, 0xe6, 0xe6);
const COLORREF kThumbHot   = RGB(0xff, 0xff, 0xff);

bool Inside(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }

void FillRound(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

void FillBox(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Text(HDC dc, const RECT& r, const wchar_t* s, COLORREF c, UINT align = DT_CENTER) {
    RECT rr = r;
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rr, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void Triangle(HDC dc, const RECT& r, bool left, COLORREF c, int size) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int hw = size / 2;
    POINT pts[3];
    if (left) {
        pts[0] = {cx + hw / 2, cy - hw};
        pts[1] = {cx + hw / 2, cy + hw};
        pts[2] = {cx - hw / 2 - 1, cy};
    } else {
        pts[0] = {cx - hw / 2, cy - hw};
        pts[1] = {cx - hw / 2, cy + hw};
        pts[2] = {cx + hw / 2 + 1, cy};
    }
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

}  // namespace

int Toolbar::Scale(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }

int Toolbar::MinClientWidth(UINT dpi) { return MulDiv(980, static_cast<int>(dpi), 96); }

void Toolbar::Layout(int clientWidth, UINT dpi) {
    dpi_ = dpi;
    height_ = Scale(44);
    bar_ = {0, 0, clientWidth, height_};

    const int ctlH = Scale(28);
    const int top = (height_ - ctlH) / 2, bottom = top + ctlH;
    auto box = [&](int x, int w) { return RECT{x, top, x + w, bottom}; };

    // Right side, laid out from the right edge.
    int x = clientWidth - Scale(12);
    next_   = box(x - Scale(32), Scale(32)); x = next_.left - Scale(4);
    prev_   = box(x - Scale(32), Scale(32)); x = prev_.left - Scale(16);
    actual_ = box(x - Scale(52), Scale(52)); x = actual_.left - Scale(4);
    fit_    = box(x - Scale(48), Scale(48)); x = fit_.left - Scale(10);
    pano_   = box(x - Scale(52), Scale(52)); x = pano_.left - Scale(4);
    fix_    = box(x - Scale(48), Scale(48));
    const int rightEdge = fix_.left - Scale(16);

    // Left side.
    x = Scale(12);
    grid_  = box(x, Scale(52)); x = grid_.right;
    image_ = box(x, Scale(60)); x = image_.right + Scale(16);
    modeGroup_ = {grid_.left, top, image_.right, bottom};
    label_ = box(x, Scale(62)); x = label_.right + Scale(6);

    // The slider takes what's left over (within limits).
    const int fixedAfterSlider = Scale(8 + 62 + 4 + 56 + 20 + 52 + 76 + 56);
    int sliderW = std::clamp(rightEdge - x - fixedAfterSlider, Scale(100), Scale(280));
    slider_ = box(x, sliderW); x = slider_.right + Scale(8);
    value_  = box(x, Scale(62)); x = value_.right + Scale(4);
    reset_  = box(x, Scale(56)); x = reset_.right + Scale(20);

    clip_     = box(x, Scale(52)); x = clip_.right;
    reinhard_ = box(x, Scale(76)); x = reinhard_.right;
    aces_     = box(x, Scale(56));
    group_    = {clip_.left, top, aces_.right, bottom};

    const int thumbR = Scale(7);
    trackLeft_  = slider_.left + thumbR;
    trackRight_ = slider_.right - thumbR;
    trackY_     = (top + bottom) / 2;
}

int Toolbar::ExposureToX(float ev) const {
    float t = std::clamp((ev - kMinExposureEV) / (kMaxExposureEV - kMinExposureEV), 0.0f, 1.0f);
    return trackLeft_ + static_cast<int>(std::lround(t * (trackRight_ - trackLeft_)));
}

float Toolbar::ExposureFromX(int x) const {
    float t = static_cast<float>(x - trackLeft_) / std::max(1, trackRight_ - trackLeft_);
    t = std::clamp(t, 0.0f, 1.0f);
    float ev = kMinExposureEV + t * (kMaxExposureEV - kMinExposureEV);
    return std::round(ev * 10.0f) / 10.0f;
}

ToolId Toolbar::HitTest(POINT p) const {
    if (!Inside(bar_, p)) return ToolId::None;
    if (Inside(grid_, p)) return ToolId::Grid;
    if (Inside(image_, p)) return ToolId::Image;
    if (Inside(slider_, p)) return ToolId::Slider;
    if (Inside(reset_, p)) return ToolId::Reset;
    if (Inside(clip_, p)) return ToolId::Clip;
    if (Inside(reinhard_, p)) return ToolId::Reinhard;
    if (Inside(aces_, p)) return ToolId::Aces;
    if (Inside(fix_, p)) return ToolId::Fix;
    if (Inside(pano_, p)) return ToolId::Pano;
    if (Inside(fit_, p)) return ToolId::Fit;
    if (Inside(actual_, p)) return ToolId::Actual;
    if (Inside(prev_, p)) return ToolId::Prev;
    if (Inside(next_, p)) return ToolId::Next;
    return ToolId::None;
}

void Toolbar::Draw(HDC dc, const ToolbarState& st, HFONT font) const {
    FillBox(dc, bar_, kBarBg);
    RECT border = {bar_.left, bar_.bottom - 1, bar_.right, bar_.bottom};
    FillBox(dc, border, kBarBorder);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font);
    const int radius = Scale(5);
    const bool on = st.hasImage;

    auto buttonColor = [&](ToolId id, bool selected) {
        if (selected) return (on && st.hover == id) ? kAccentHot : kAccent;
        if (!on) return kButton;
        if (st.pressed == id) return kButtonDown;
        if (st.hover == id) return kButtonHot;
        return kButton;
    };
    auto button = [&](const RECT& r, ToolId id, const wchar_t* label, bool selected) {
        FillRound(dc, r, buttonColor(id, selected), radius);
        Text(dc, r, label, on ? kText : kTextOff);
    };

    // Grid | Image switch.
    {
        FillRound(dc, modeGroup_, kButton, radius);
        const int inset = std::max(2, Scale(2));
        struct Seg { const RECT* r; ToolId id; bool active; bool enabled; const wchar_t* label; };
        const Seg segs[] = {{&grid_, ToolId::Grid, st.gridMode, st.canGrid, L"Grid"},
                            {&image_, ToolId::Image, !st.gridMode, st.canImage, L"Image"}};
        for (const Seg& sg : segs) {
            RECT rr = *sg.r;
            InflateRect(&rr, -inset, -inset);
            if (sg.active && (sg.enabled || sg.id == ToolId::Image))
                FillRound(dc, rr, (sg.enabled && st.hover == sg.id) ? kAccentHot : kAccent, radius - 1);
            else if (sg.enabled && (st.hover == sg.id || st.pressed == sg.id))
                FillRound(dc, rr, st.pressed == sg.id ? kButtonDown : kButtonHot, radius - 1);
            Text(dc, *sg.r, sg.label, (sg.enabled || sg.active) ? kText : kTextOff);
        }
    }

    // Exposure label, slider, value, reset.
    Text(dc, label_, L"Exposure", on ? kTextDim : kTextOff, DT_LEFT);

    const int trackH = std::max(2, Scale(4));
    RECT track = {trackLeft_, trackY_ - trackH / 2, trackRight_, trackY_ - trackH / 2 + trackH};
    FillRound(dc, track, kTrack, trackH / 2);

    const int zeroX = ExposureToX(0.0f);
    const int thumbX = ExposureToX(st.exposure);
    if (thumbX != zeroX) {
        RECT fill = {std::min(zeroX, thumbX), track.top, std::max(zeroX, thumbX), track.bottom};
        FillBox(dc, fill, on ? kAccent : kTrack);
    }
    RECT tick = {zeroX - std::max(1, Scale(1)), trackY_ - Scale(6), zeroX + std::max(1, Scale(1)), trackY_ + Scale(6)};
    FillBox(dc, tick, RGB(0x70, 0x70, 0x70));

    const int r = Scale(7);
    HBRUSH tb = CreateSolidBrush(!on ? kTrack
                                     : (st.hover == ToolId::Slider || st.pressed == ToolId::Slider) ? kThumbHot : kThumb);
    HGDIOBJ ob = SelectObject(dc, tb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, thumbX - r, trackY_ - r, thumbX + r + 1, trackY_ + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(tb);

    wchar_t val[32];
    float ev = std::fabs(st.exposure) < 0.05f ? 0.0f : st.exposure;
    if (ev == 0.0f) std::swprintf(val, std::size(val), L"0.0 EV");
    else std::swprintf(val, std::size(val), L"%+.1f EV", ev);
    Text(dc, value_, val, on ? kText : kTextOff, DT_LEFT);

    button(reset_, ToolId::Reset, L"Reset", false);

    // Tonemap choice: one rounded group, the active one highlighted.
    FillRound(dc, group_, kButton, radius);
    struct Seg { const RECT* r; ToolId id; Tonemap curve; const wchar_t* label; };
    const Seg segs[] = {{&clip_, ToolId::Clip, Tonemap::Clip, L"Clip"},
                        {&reinhard_, ToolId::Reinhard, Tonemap::Reinhard, L"Reinhard"},
                        {&aces_, ToolId::Aces, Tonemap::ACES, L"ACES"}};
    const int inset = std::max(2, Scale(2));
    for (const Seg& s : segs) {
        bool selected = st.curve == s.curve;
        RECT rr = *s.r;
        InflateRect(&rr, -inset, -inset);
        if (selected) FillRound(dc, rr, on ? kAccent : kButtonDown, radius - 1);
        else if (on && (st.hover == s.id || st.pressed == s.id))
            FillRound(dc, rr, st.pressed == s.id ? kButtonDown : kButtonHot, radius - 1);
        Text(dc, *s.r, s.label, on ? kText : kTextOff);
    }

    // View buttons (single image only).
    {
        const bool v = st.viewControls;
        auto viewButton = [&](const RECT& r, ToolId id, const wchar_t* label, bool selected) {
            COLORREF bg = selected ? ((v && st.hover == id) ? kAccentHot : kAccent)
                        : !v ? kButton : (st.pressed == id) ? kButtonDown : (st.hover == id) ? kButtonHot : kButton;
            FillRound(dc, r, bg, radius);
            Text(dc, r, label, v ? kText : kTextOff);
        };
        viewButton(fit_, ToolId::Fit, L"Fit", v && st.fit && !st.pano);
        viewButton(actual_, ToolId::Actual, L"100%", v && st.actualSize && !st.fit && !st.pano);

        // 360-degree toggle: highlighted while on, dimmed when the image isn't a 2:1 panorama.
        const bool pv = v && st.canPano;
        COLORREF bg = (pv && st.pano) ? (st.hover == ToolId::Pano ? kAccentHot : kAccent)
                    : !pv ? kButton : (st.pressed == ToolId::Pano) ? kButtonDown
                    : (st.hover == ToolId::Pano) ? kButtonHot : kButton;
        FillRound(dc, pano_, bg, radius);
        Text(dc, pano_, L"360\u00b0", pv ? kText : kTextOff);

        // Fix HDRI: highlighted while a fix is applied.
        const bool fv = v && st.canFix;
        COLORREF fb = (fv && st.fixOn) ? (st.hover == ToolId::Fix ? kAccentHot : kAccent)
                    : !fv ? kButton : (st.pressed == ToolId::Fix) ? kButtonDown
                    : (st.hover == ToolId::Fix) ? kButtonHot : kButton;
        FillRound(dc, fix_, fb, radius);
        Text(dc, fix_, L"Fix", fv ? kText : kTextOff);
    }

    // Previous / next file.
    auto arrow = [&](const RECT& rr, ToolId id, bool enabled, bool left) {
        COLORREF bg = (!enabled) ? kButton : (st.pressed == id) ? kButtonDown : (st.hover == id) ? kButtonHot : kButton;
        FillRound(dc, rr, bg, radius);
        Triangle(dc, rr, left, enabled ? kText : kTextOff, Scale(10));
    };
    arrow(prev_, ToolId::Prev, st.canPrev, true);
    arrow(next_, ToolId::Next, st.canNext, false);

    SelectObject(dc, oldFont);
}
