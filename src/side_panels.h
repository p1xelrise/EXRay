// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The two side panels of the Studio layout, custom drawn like the toolbar:
//  - the sidebar on the left: search scope, the folder library as a tree,
//    "+ Add" and the library status;
//  - the inspector on the right: details of the selected file and its actions.
// The search box itself is a real edit control placed by the main window
// over SearchBoxRect().
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "library.h"

class Bitmap;

struct PanelFonts {
    HFONT body = nullptr;    // 13 px
    HFONT bold = nullptr;    // 13 px semibold
    HFONT small = nullptr;   // 12 px
    HFONT title = nullptr;   // 15 px semibold
};

// Panel colours (shared with the main window).
namespace panel {
const COLORREF kPanelBg   = RGB(0x1b, 0x1c, 0x20);
const COLORREF kLine      = RGB(0x2a, 0x2c, 0x31);
const COLORREF kText      = RGB(0xe8, 0xe9, 0xec);
const COLORREF kSoft      = RGB(0xc9, 0xcb, 0xd1);
const COLORREF kDim       = RGB(0x9a, 0x9c, 0xa3);
const COLORREF kOff       = RGB(0x5e, 0x61, 0x68);
const COLORREF kButton    = RGB(0x2b, 0x2d, 0x33);
const COLORREF kButtonHot = RGB(0x36, 0x39, 0x40);
const COLORREF kRowHot    = RGB(0x24, 0x26, 0x2b);
const COLORREF kRowSel    = RGB(0x2e, 0x31, 0x38);
const COLORREF kAccent    = RGB(0x3d, 0x7f, 0xe0);
const COLORREF kAccentHot = RGB(0x4d, 0x8f, 0xf0);
const COLORREF kTile      = RGB(0x22, 0x23, 0x28);
const COLORREF kWarn      = RGB(0xf0, 0xa6, 0x40);
const COLORREF kDanger    = RGB(0xff, 0x9a, 0x9a);
}  // namespace panel

// --- sidebar --------------------------------------------------------------------------

enum class SidebarPart { None, ScopeThis, ScopeAll, Add, Row, RowChevron, RowRemove, EmptyAdd };

struct SidebarHit {
    SidebarPart part = SidebarPart::None;
    int row = -1;
    bool operator==(const SidebarHit& o) const { return part == o.part && row == o.row; }
    bool operator!=(const SidebarHit& o) const { return !(*this == o); }
};

struct SidebarState {
    const std::vector<TreeRow>* rows = nullptr;
    int current = -1;               // row of the folder shown in the grid
    std::vector<int> matches;       // per row: search matches (empty when not searching)
    bool scopeAll = true;           // search all folders (else the folder in the grid)
    bool scopeThisEnabled = false;  // a folder is shown, so "This folder" can be picked
    bool searching = false;
    std::wstring status;            // "179 files · 3 folders"
    SidebarHit hover;
};

class Sidebar {
public:
    // `area` is the sidebar's rectangle in client coordinates.
    void Layout(const RECT& area, UINT dpi, int rowCount);
    const RECT& Area() const { return area_; }
    RECT SearchBoxRect() const { return search_; }

    void Draw(HDC dc, const SidebarState& st, const PanelFonts& f) const;
    SidebarHit HitTest(POINT p, const SidebarState& st) const;

    // Rows scroll; returns true when the position changed.
    bool ScrollBy(int dy);
    void EnsureRowVisible(int row);
    RECT RowRect(int row) const;    // in client coordinates (may be outside the list)
    int RowHeight() const { return rowH_; }

    static int Width(UINT dpi) { return MulDiv(264, static_cast<int>(dpi), 96); }

private:
    int S(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    UINT dpi_ = 96;
    RECT area_{}, search_{}, scopeThis_{}, scopeAll_{}, header_{}, add_{}, list_{}, status_{}, emptyAdd_{};
    int rowH_ = 30, rows_ = 0, scroll_ = 0, maxScroll_ = 0;
};

// --- inspector --------------------------------------------------------------------------

struct InspectorButton {
    std::wstring label;
    int id = 0;              // what the main window does on a click
    bool primary = false;
    bool enabled = true;
};

struct InspectorStat {
    std::wstring label, value;
    bool warn = false;
};

struct InspectorInfo {
    std::wstring emptyText;          // shown when nothing is selected
    const Bitmap* preview = nullptr; // optional preview image
    std::wstring title;              // file name
    std::vector<std::wstring> lines; // details under the name
    std::wstring path;               // folder, drawn small and dim
    std::vector<InspectorStat> stats;
    std::wstring note;               // small text under the stats
    std::vector<InspectorButton> buttons;
};

class Inspector {
public:
    void Layout(const RECT& area, UINT dpi) { area_ = area; dpi_ = dpi; }
    const RECT& Area() const { return area_; }
    // Draws and remembers where the buttons ended up (for HitTest).
    void Draw(HDC dc, const InspectorInfo& info, const PanelFonts& f, int hoverId, int pressedId);
    int HitTest(POINT p) const;      // button id or 0

    static int Width(UINT dpi) { return MulDiv(316, static_cast<int>(dpi), 96); }

private:
    int S(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    UINT dpi_ = 96;
    RECT area_{};
    struct Placed { RECT r; int id; bool enabled; };
    std::vector<Placed> placed_;
};

// Small drawing helpers shared by the panels and the main window.
void PanelFill(HDC dc, const RECT& r, COLORREF c);
void PanelRound(HDC dc, const RECT& r, COLORREF fill, int radius);
void PanelText(HDC dc, HFONT f, COLORREF c, const std::wstring& t, RECT r, UINT fmt);
int PanelTextHeight(HDC dc, HFONT f, const std::wstring& t, int width);
