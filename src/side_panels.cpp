// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "side_panels.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "display_win.h"

using namespace panel;

// --- helpers ------------------------------------------------------------------------------

void PanelFill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void PanelRound(HDC dc, const RECT& r, COLORREF fill, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

void PanelText(HDC dc, HFONT f, COLORREF c, const std::wstring& t, RECT r, UINT fmt) {
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

int PanelTextHeight(HDC dc, HFONT f, const std::wstring& t, int width) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, std::max(1, width), 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

namespace {

// A small folder outline, `s` pixels wide, its top-left at (x, y).
void DrawFolderIcon(HDC dc, int x, int y, int s, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, s / 9), c);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int h = s * 3 / 4, tab = s * 2 / 5;
    POINT pts[] = {{x, y + s / 8}, {x + tab, y + s / 8}, {x + tab + s / 8, y + s / 4}, {x + s, y + s / 4},
                   {x + s, y + s / 8 + h}, {x, y + s / 8 + h}, {x, y + s / 8}};
    Polyline(dc, pts, static_cast<int>(std::size(pts)));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// A filled triangle: pointing right (closed folder) or down (open).
void DrawChevron(HDC dc, int cx, int cy, int s, bool open, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    if (open) {
        POINT pts[] = {{cx - s, cy - s / 2}, {cx + s, cy - s / 2}, {cx, cy + s / 2 + 1}};
        Polygon(dc, pts, 3);
    } else {
        POINT pts[] = {{cx - s / 2, cy - s}, {cx + s / 2 + 1, cy}, {cx - s / 2, cy + s}};
        Polygon(dc, pts, 3);
    }
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

void DrawCross(HDC dc, const RECT& r, COLORREF c, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ op = SelectObject(dc, pen);
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, a = (r.bottom - r.top) / 5;
    MoveToEx(dc, cx - a, cy - a, nullptr); LineTo(dc, cx + a + 1, cy + a + 1);
    MoveToEx(dc, cx + a, cy - a, nullptr); LineTo(dc, cx - a - 1, cy + a + 1);
    SelectObject(dc, op);
    DeleteObject(pen);
}

std::wstring Count(int n) {
    std::wstring s = std::to_wstring(n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        const size_t left = s.size() - 1 - i;
        if (left > 0 && left % 3 == 0) out += L',';
    }
    return out;
}

}  // namespace

// --- sidebar --------------------------------------------------------------------------

void Sidebar::Layout(const RECT& area, UINT dpi, int rowCount) {
    area_ = area;
    dpi_ = dpi;
    const int pad = S(12);
    const int x0 = area.left + pad, x1 = area.right - pad;
    search_ = {x0, area.top + S(12), x1, area.top + S(12) + S(30)};
    const int half = (x1 - x0) / 2;
    scopeThis_ = {x0, search_.bottom + S(8), x0 + half, search_.bottom + S(8) + S(26)};
    scopeAll_ = {x0 + half, scopeThis_.top, x1, scopeThis_.bottom};
    header_ = {x0, scopeThis_.bottom + S(18), x1, scopeThis_.bottom + S(18) + S(24)};
    add_ = {x1 - S(64), header_.top, x1, header_.bottom};
    status_ = {x0, area.bottom - S(12) - S(40), x1, area.bottom - S(12)};
    list_ = {area.left + S(6), header_.bottom + S(8), area.right - S(6), std::max<LONG>(header_.bottom + S(8), status_.top - S(10))};
    rowH_ = S(30);
    rows_ = rowCount;
    emptyAdd_ = {x0, list_.top + S(64), x1, list_.top + S(64) + S(34)};
    const int listH = list_.bottom - list_.top;
    maxScroll_ = std::max(0, rows_ * rowH_ - listH);
    scroll_ = std::clamp(scroll_, 0, maxScroll_);
}

bool Sidebar::ScrollBy(int dy) {
    const int s = std::clamp(scroll_ + dy, 0, maxScroll_);
    if (s == scroll_) return false;
    scroll_ = s;
    return true;
}

RECT Sidebar::RowRect(int row) const {
    const int top = list_.top + row * rowH_ - scroll_;
    return RECT{list_.left, top, list_.right, top + rowH_};
}

void Sidebar::EnsureRowVisible(int row) {
    if (row < 0) return;
    const int top = row * rowH_, bottom = top + rowH_, listH = list_.bottom - list_.top;
    if (top < scroll_) scroll_ = top;
    else if (bottom > scroll_ + listH) scroll_ = bottom - listH;
    scroll_ = std::clamp(scroll_, 0, maxScroll_);
}

SidebarHit Sidebar::HitTest(POINT p, const SidebarState& st) const {
    SidebarHit h;
    if (!PtInRect(&area_, p)) return h;
    if (PtInRect(&scopeThis_, p) && st.scopeThisEnabled) { h.part = SidebarPart::ScopeThis; return h; }
    if (PtInRect(&scopeAll_, p)) { h.part = SidebarPart::ScopeAll; return h; }
    if (PtInRect(&add_, p)) { h.part = SidebarPart::Add; return h; }
    const int n = st.rows ? static_cast<int>(st.rows->size()) : 0;
    if (n == 0) {
        if (PtInRect(&emptyAdd_, p)) h.part = SidebarPart::EmptyAdd;
        return h;
    }
    if (!PtInRect(&list_, p)) return h;
    const int row = (p.y - list_.top + scroll_) / rowH_;
    if (row < 0 || row >= n) return h;
    const TreeRow& tr = (*st.rows)[static_cast<size_t>(row)];
    const RECT r = RowRect(row);
    h.row = row;
    h.part = SidebarPart::Row;
    const int indent = r.left + S(6) + tr.depth * S(14);
    if (tr.hasChildren && p.x >= indent - S(4) && p.x < indent + S(16)) h.part = SidebarPart::RowChevron;
    if (tr.depth == 0 && p.x >= r.right - S(30)) h.part = SidebarPart::RowRemove;
    return h;
}

void Sidebar::Draw(HDC dc, const SidebarState& st, const PanelFonts& f) const {
    PanelFill(dc, area_, kPanelBg);
    RECT edge{area_.right - 1, area_.top, area_.right, area_.bottom};
    PanelFill(dc, edge, kLine);

    // The search box frame and its magnifier (the edit control sits inside it).
    PanelRound(dc, search_, kButton, S(7));
    {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2) - 1), kDim);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        const int cx = search_.left + S(15), cy = (search_.top + search_.bottom) / 2 - S(1), r = S(5);
        Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
        MoveToEx(dc, cx + r - 1, cy + r - 1, nullptr);
        LineTo(dc, cx + r + S(4), cy + r + S(4));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    // Search scope: This folder | All folders.
    RECT group{scopeThis_.left, scopeThis_.top, scopeAll_.right, scopeAll_.bottom};
    PanelRound(dc, group, kButton, S(6));
    auto seg = [&](const RECT& r, const wchar_t* label, bool on, bool enabled, bool hot) {
        RECT in = r;
        InflateRect(&in, -S(2), -S(2));
        if (on) PanelRound(dc, in, hot ? kAccentHot : kAccent, S(5));
        else if (hot && enabled) PanelRound(dc, in, kButtonHot, S(5));
        PanelText(dc, f.small, !enabled ? kOff : on ? RGB(0xff, 0xff, 0xff) : kSoft, label, r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    seg(scopeThis_, L"This folder", !st.scopeAll && st.scopeThisEnabled, st.scopeThisEnabled,
        st.hover.part == SidebarPart::ScopeThis);
    seg(scopeAll_, L"All folders", st.scopeAll || !st.scopeThisEnabled, true, st.hover.part == SidebarPart::ScopeAll);

    // FOLDERS   [+ Add]
    PanelText(dc, f.small, kDim, L"FOLDERS", RECT{header_.left + S(2), header_.top, add_.left, header_.bottom},
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    PanelRound(dc, add_, st.hover.part == SidebarPart::Add ? kButtonHot : kButton, S(6));
    PanelText(dc, f.small, kText, L"+ Add", add_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int n = st.rows ? static_cast<int>(st.rows->size()) : 0;
    if (n == 0) {
        RECT msg{emptyAdd_.left, list_.top + S(4), emptyAdd_.right, emptyAdd_.top - S(8)};
        PanelText(dc, f.small, kDim, L"Add the folders where you keep your HDRIs and EXR renders. They stay here for next time.",
                  msg, DT_LEFT | DT_WORDBREAK);
        PanelRound(dc, emptyAdd_, st.hover.part == SidebarPart::EmptyAdd ? kAccentHot : kAccent, S(6));
        PanelText(dc, f.bold, RGB(0xff, 0xff, 0xff), L"+ Add a folder", emptyAdd_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, list_.left, list_.top, list_.right, list_.bottom);
        for (int i = 0; i < n; ++i) {
            const RECT r = RowRect(i);
            if (r.bottom <= list_.top || r.top >= list_.bottom) continue;
            const TreeRow& tr = (*st.rows)[static_cast<size_t>(i)];
            const bool hot = st.hover.row == i && st.hover.part != SidebarPart::None;
            if (i == st.current) PanelRound(dc, r, kRowSel, S(6));
            else if (hot) PanelRound(dc, r, kRowHot, S(6));

            const int cy = (r.top + r.bottom) / 2;
            int x = r.left + S(6) + tr.depth * S(14);
            if (tr.hasChildren)
                DrawChevron(dc, x + S(5), cy, S(4), tr.expanded,
                            st.hover.row == i && st.hover.part == SidebarPart::RowChevron ? kText : kDim);
            x += S(14);
            const COLORREF iconC = tr.missing ? kOff : i == st.current ? kAccent : tr.depth == 0 ? kSoft : kDim;
            DrawFolderIcon(dc, x, cy - S(7), S(15), iconC);
            x += S(15) + S(7);

            // Right side: search matches, the remove button on hover, or the file count.
            int right = r.right - S(8);
            const bool searchRow = st.searching && i < static_cast<int>(st.matches.size());
            if (tr.depth == 0 && hot) {
                RECT rm{r.right - S(28), r.top + S(4), r.right - S(4), r.bottom - S(4)};
                if (st.hover.part == SidebarPart::RowRemove) PanelRound(dc, rm, kButtonHot, S(5));
                DrawCross(dc, rm, st.hover.part == SidebarPart::RowRemove ? kDanger : kDim, std::max(1, S(1)));
                right = rm.left - S(4);
            } else if (searchRow) {
                const int m = st.matches[static_cast<size_t>(i)];
                if (m > 0) {
                    const std::wstring t = Count(m);
                    const int w = S(10) + static_cast<int>(t.size()) * S(7);
                    RECT pill{r.right - S(8) - w, cy - S(9), r.right - S(8), cy + S(9)};
                    PanelRound(dc, pill, kAccent, S(9));
                    PanelText(dc, f.small, RGB(0xff, 0xff, 0xff), t, pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    right = pill.left - S(6);
                }
            } else if (tr.scanned) {
                RECT cr{r.right - S(56), r.top, r.right - S(8), r.bottom};
                PanelText(dc, f.small, kDim, Count(tr.count), cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                right = cr.left - S(4);
            }

            std::wstring name = tr.name;
            COLORREF nameC = i == st.current ? kText : tr.depth == 0 ? kText : kSoft;
            if (tr.missing) { name += L"  (not found)"; nameC = kOff; }
            else if (searchRow && st.matches[static_cast<size_t>(i)] == 0) nameC = kOff;
            RECT nr{x, r.top, std::max<LONG>(x, right), r.bottom};
            PanelText(dc, tr.depth == 0 || i == st.current ? f.bold : f.body, nameC, name, nr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | (tr.depth == 0 ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
        }
        RestoreDC(dc, saved);
        // A thin scroll indicator.
        if (maxScroll_ > 0) {
            const int listH = list_.bottom - list_.top;
            const int thumbH = std::max(S(24), listH * listH / (listH + maxScroll_));
            const int ty = list_.top + (listH - thumbH) * scroll_ / std::max(1, maxScroll_);
            RECT t{area_.right - S(5), ty, area_.right - S(2), ty + thumbH};
            PanelRound(dc, t, kButtonHot, S(1));
        }
    }

    if (!st.status.empty()) {
        PanelRound(dc, status_, kTile, S(8));
        RECT tr = status_;
        InflateRect(&tr, -S(10), 0);
        PanelText(dc, f.small, kDim, st.status, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// --- inspector --------------------------------------------------------------------------

void Inspector::Draw(HDC dc, const InspectorInfo& info, const PanelFonts& f, int hoverId, int pressedId) {
    PanelFill(dc, area_, kPanelBg);
    RECT edge{area_.left, area_.top, area_.left + 1, area_.bottom};
    PanelFill(dc, edge, kLine);
    placed_.clear();

    const int pad = S(16);
    const int x0 = area_.left + pad, x1 = area_.right - pad, inner = x1 - x0;
    int y = area_.top + pad;

    if (info.title.empty()) {
        RECT r{x0, y, x1, area_.bottom - pad};
        PanelText(dc, f.small, kDim, info.emptyText, r, DT_LEFT | DT_WORDBREAK);
        return;
    }

    if (info.preview && info.preview->width() > 0 && info.preview->height() > 0) {
        const Bitmap& b = *info.preview;
        int w = inner, h = static_cast<int>(static_cast<long long>(inner) * b.height() / b.width());
        if (h > inner) { h = inner; w = static_cast<int>(static_cast<long long>(inner) * b.width() / b.height()); }
        const int px = x0 + (inner - w) / 2;
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, b.handle());
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, px, y, w, h, mem, 0, 0, b.width(), b.height(), SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        y += h + S(14);
    }

    const int th = PanelTextHeight(dc, f.title, info.title, inner);
    PanelText(dc, f.title, kText, info.title, RECT{x0, y, x1, y + th}, DT_LEFT | DT_WORDBREAK);
    y += th + S(4);
    for (const auto& line : info.lines) {
        const int lh = PanelTextHeight(dc, f.body, line, inner);
        PanelText(dc, f.body, kDim, line, RECT{x0, y, x1, y + lh}, DT_LEFT | DT_WORDBREAK);
        y += lh + S(2);
    }
    if (!info.path.empty()) {
        const int lh = PanelTextHeight(dc, f.small, L"Ag", inner);
        PanelText(dc, f.small, kDim, info.path, RECT{x0, y, x1, y + lh}, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        y += lh;
    }

    if (!info.stats.empty()) {
        y += S(14);
        const int gap = S(8), tw = (inner - gap) / 2, tileH = S(54);
        for (size_t i = 0; i < info.stats.size(); ++i) {
            const int col = static_cast<int>(i % 2);
            const int tx = x0 + col * (tw + gap);
            RECT t{tx, y, tx + tw, y + tileH};
            PanelRound(dc, t, kTile, S(8));
            PanelText(dc, f.small, kDim, info.stats[i].label, RECT{t.left + S(10), t.top + S(7), t.right - S(6), t.top + S(25)},
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            PanelText(dc, f.title, info.stats[i].warn ? kWarn : kText, info.stats[i].value,
                      RECT{t.left + S(10), t.top + S(25), t.right - S(6), t.bottom - S(6)}, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (col == 1 || i + 1 == info.stats.size()) y += tileH + gap;
        }
        y -= gap;
    }
    if (!info.note.empty()) {
        y += S(8);
        const int lh = PanelTextHeight(dc, f.small, info.note, inner);
        PanelText(dc, f.small, kDim, info.note, RECT{x0, y, x1, y + lh}, DT_LEFT | DT_WORDBREAK);
        y += lh;
    }

    if (!info.buttons.empty()) {
        y += S(18);
        const int bh = S(34);
        for (const auto& b : info.buttons) {
            RECT r{x0, y, x1, y + bh};
            if (r.bottom > area_.bottom - S(8)) break;   // a very short window: no room
            const bool hot = b.enabled && hoverId == b.id, down = b.enabled && pressedId == b.id;
            COLORREF fill = b.primary ? (hot || down ? kAccentHot : kAccent) : (hot || down ? kButtonHot : kButton);
            if (!b.enabled) fill = kButton;
            PanelRound(dc, r, fill, S(7));
            PanelText(dc, b.primary ? f.bold : f.body, !b.enabled ? kOff : b.primary ? RGB(0xff, 0xff, 0xff) : kText, b.label, r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            placed_.push_back({r, b.id, b.enabled});
            y += bh + S(8);
        }
    }
}

int Inspector::HitTest(POINT p) const {
    for (const auto& b : placed_)
        if (b.enabled && PtInRect(&b.r, p)) return b.id;
    return 0;
}
