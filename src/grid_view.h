// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder grid: layout, scrolling, drawing and hit testing.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "display_win.h"
#include "thumbnail.h"
#include "tonemap.h"

struct GridItem {
    enum class State { Waiting, Done, Failed };

    std::wstring path;
    std::wstring name;
    uint64_t fileSize = 0;
    uint64_t writeTime = 0;        // to notice changed files on refresh

    State state = State::Waiting;
    bool picked = false;           // part of the multi-selection
    int imageWidth = 0, imageHeight = 0;
    std::string format;
    std::string error;
    Thumbnail thumb;

    // Tonemapped copy for drawing; only kept for tiles on or near the screen.
    std::unique_ptr<Bitmap> bmp;
    TonemapSettings bmpSettings;
};

struct GridFonts { HFONT name; HFONT detail; };

class GridView {
public:
    // Thumbnail box size (312 x 156 at 100% Windows scaling and 100% grid size)
    // and spacing for this DPI and grid size (0.5 .. 2.0).
    void SetDpi(UINT dpi, float scale = 1.0f);
    int BoxWidth() const { return boxW_; }
    int BoxHeight() const { return boxH_; }

    // Recomputes columns and scroll limits for the viewport and item count.
    void Layout(const RECT& viewport, int itemCount);

    int Columns() const { return cols_; }
    int RowsPerPage() const;
    int ScrollY() const { return scrollY_; }
    void ScrollTo(int y);
    void ScrollBy(int dy) { ScrollTo(scrollY_ + dy); }
    int WheelStep() const { return tileH_ / 2 + gap_; }

    // Range of items at least partly on screen (last < first when none).
    void VisibleRange(int& first, int& last) const;

    // Item index under a point, or -1.
    int HitTest(POINT p) const;
    RECT ItemRect(int index) const;      // in client coordinates
    void EnsureVisible(int index);

    // Scrollbar (drawn inside the viewport's right edge).
    bool HasScrollbar() const { return maxScroll_ > 0; }
    bool InScrollbar(POINT p) const;
    RECT ScrollThumbRect() const;
    // Converts a thumb drag (mouse y relative to where it was grabbed) into a scroll position.
    void DragThumbTo(int thumbTop);

    // Draws the visible tiles. Creates the tonemapped bitmaps it needs and
    // frees those that are far off screen.
    void Draw(HDC dc, const RECT& viewport, std::vector<GridItem>& items, const TonemapSettings& s,
              int focus, int hover, bool scrollHot, const GridFonts& fonts);

private:
    int Scale(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }

    UINT dpi_ = 96;
    int boxW_ = 312, boxH_ = 156;
    int pad_ = 8, gap_ = 12, margin_ = 16, textH_ = 40;
    int tileW_ = 0, tileH_ = 0;

    RECT vp_{};
    int count_ = 0;
    int cols_ = 1, rows_ = 0;
    int left_ = 0;             // x of the first column
    int contentH_ = 0;
    int scrollY_ = 0, maxScroll_ = 0;
};
