// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "grid_view.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace {

const COLORREF kTileHover    = RGB(0x2a, 0x2a, 0x2a);
const COLORREF kTileSelected = RGB(0x26, 0x36, 0x4a);
const COLORREF kAccent       = RGB(0x3a, 0x7b, 0xc8);
const COLORREF kBoxBg        = RGB(0x26, 0x26, 0x26);
const COLORREF kName         = RGB(0xdd, 0xdd, 0xdd);
const COLORREF kDetail       = RGB(0x8a, 0x8a, 0x8a);
const COLORREF kFaint        = RGB(0x5e, 0x5e, 0x5e);
const COLORREF kScroll       = RGB(0x4a, 0x4a, 0x4a);
const COLORREF kScrollHot    = RGB(0x70, 0x70, 0x70);

void FillRound(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

void FrameRound(HDC dc, const RECT& r, COLORREF c, int radius, int width) {
    HPEN pen = CreatePen(PS_INSIDEFRAME, width, c);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void Text(HDC dc, RECT r, const std::wstring& s, COLORREF c, UINT flags) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (unsigned char ch : s) w += static_cast<wchar_t>(ch);   // formats/errors are ASCII
    return w;
}

}  // namespace

void GridView::SetDpi(UINT dpi, float scale) {
    dpi_ = dpi;
    boxW_ = std::max(16, static_cast<int>(Scale(312) * scale + 0.5f));
    boxH_ = std::max(8, static_cast<int>(Scale(156) * scale + 0.5f));
    pad_ = Scale(8);
    gap_ = Scale(12);
    margin_ = Scale(16);
    textH_ = Scale(44);
    tileW_ = boxW_ + 2 * pad_;
    tileH_ = pad_ + boxH_ + textH_ + pad_;
}

void GridView::Layout(const RECT& viewport, int itemCount) {
    vp_ = viewport;
    count_ = itemCount;
    const int vpW = vp_.right - vp_.left, vpH = vp_.bottom - vp_.top;
    cols_ = std::max(1, (vpW - 2 * margin_ + gap_) / (tileW_ + gap_));
    rows_ = (count_ + cols_ - 1) / cols_;
    const int gridW = cols_ * tileW_ + (cols_ - 1) * gap_;
    left_ = vp_.left + std::max(margin_, (vpW - gridW) / 2);
    contentH_ = rows_ > 0 ? 2 * margin_ + rows_ * tileH_ + (rows_ - 1) * gap_ : 0;
    maxScroll_ = std::max(0, contentH_ - vpH);
    scrollY_ = std::clamp(scrollY_, 0, maxScroll_);
}

int GridView::RowsPerPage() const {
    return std::max(1, static_cast<int>((vp_.bottom - vp_.top) / (tileH_ + gap_)));
}

void GridView::ScrollTo(int y) { scrollY_ = std::clamp(y, 0, maxScroll_); }

void GridView::VisibleRange(int& first, int& last) const {
    first = 0;
    last = -1;
    if (count_ == 0) return;
    const int rowStep = tileH_ + gap_;
    const int vpH = vp_.bottom - vp_.top;
    const int firstRow = std::max(0, (scrollY_ - margin_) / rowStep);
    const int lastRow = std::max(0, (scrollY_ + vpH - margin_) / rowStep);
    first = std::min(count_ - 1, firstRow * cols_);
    last = std::min(count_ - 1, (lastRow + 1) * cols_ - 1);
}

RECT GridView::ItemRect(int index) const {
    const int row = index / cols_, col = index % cols_;
    const int x = left_ + col * (tileW_ + gap_);
    const int y = vp_.top + margin_ + row * (tileH_ + gap_) - scrollY_;
    return {x, y, x + tileW_, y + tileH_};
}

int GridView::HitTest(POINT p) const {
    if (!PtInRect(&vp_, p) || count_ == 0) return -1;
    const int x = p.x - left_;
    const int y = p.y - vp_.top - margin_ + scrollY_;
    if (x < 0 || y < 0) return -1;
    const int col = x / (tileW_ + gap_), row = y / (tileH_ + gap_);
    if (col >= cols_ || x % (tileW_ + gap_) >= tileW_ || y % (tileH_ + gap_) >= tileH_) return -1;
    const int i = row * cols_ + col;
    return i < count_ ? i : -1;
}

void GridView::EnsureVisible(int index) {
    if (index < 0 || index >= count_) return;
    RECT r = ItemRect(index);
    if (r.top < vp_.top + gap_) ScrollBy(r.top - (vp_.top + gap_));
    else if (r.bottom > vp_.bottom - gap_) ScrollBy(r.bottom - (vp_.bottom - gap_));
}

RECT GridView::ScrollThumbRect() const {
    RECT r{};
    if (maxScroll_ <= 0) return r;
    const int top = vp_.top + Scale(4), bottom = vp_.bottom - Scale(4);
    const int trackH = std::max(1, bottom - top);
    const int vpH = vp_.bottom - vp_.top;
    const int thumbH = std::clamp(static_cast<int>(static_cast<long long>(trackH) * vpH / std::max(1, contentH_)),
                                  Scale(32), trackH);
    const int thumbTop = top + static_cast<int>(static_cast<long long>(trackH - thumbH) * scrollY_ / maxScroll_);
    r = {vp_.right - Scale(11), thumbTop, vp_.right - Scale(4), thumbTop + thumbH};
    return r;
}

bool GridView::InScrollbar(POINT p) const {
    if (maxScroll_ <= 0) return false;
    return p.x >= vp_.right - Scale(16) && p.x < vp_.right && p.y >= vp_.top && p.y < vp_.bottom;
}

void GridView::DragThumbTo(int thumbTop) {
    RECT t = ScrollThumbRect();
    const int top = vp_.top + Scale(4), bottom = vp_.bottom - Scale(4);
    const int range = std::max(1, static_cast<int>((bottom - top) - (t.bottom - t.top)));
    ScrollTo(static_cast<int>(static_cast<long long>(thumbTop - top) * maxScroll_ / range));
}

void GridView::Draw(HDC dc, const RECT& viewport, std::vector<GridItem>& items, const TonemapSettings& s,
                    int focus, int hover, bool scrollHot, const GridFonts& fonts) {
    int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    SetBkMode(dc, TRANSPARENT);
    HDC memDC = CreateCompatibleDC(dc);

    int first, last;
    VisibleRange(first, last);
    const int radius = Scale(6);

    for (int i = first; i <= last; ++i) {
        GridItem& it = items[static_cast<size_t>(i)];
        const RECT r = ItemRect(i);

        if (it.picked) {
            FillRound(dc, r, kTileSelected, radius);
            if (i == focus) FrameRound(dc, r, kAccent, radius, std::max(1, Scale(2)));
        } else {
            if (i == hover) FillRound(dc, r, kTileHover, radius);
            if (i == focus) FrameRound(dc, r, RGB(0x5a, 0x5a, 0x5a), radius, std::max(1, Scale(1)));
        }

        RECT box = {r.left + pad_, r.top + pad_, r.left + pad_ + boxW_, r.top + pad_ + boxH_};
        HBRUSH bb = CreateSolidBrush(kBoxBg);
        FillRect(dc, &box, bb);
        DeleteObject(bb);

        std::wstring detail;
        if (it.state == GridItem::State::Done && !it.thumb.empty()) {
            // (Re)make the 8-bit version when missing or when exposure/tonemap changed.
            try {
                bool fresh = false;
                if (!it.bmp || it.bmp->width() != it.thumb.width || it.bmp->height() != it.thumb.height) {
                    it.bmp = Bitmap::Create(it.thumb.width, it.thumb.height);
                    fresh = true;
                }
                if (fresh || it.bmpSettings != s) {
                    TonemapThumbnail(it.thumb, s, it.bmp->bits());
                    GdiFlush();
                    it.bmpSettings = s;
                }
            } catch (...) { it.bmp.reset(); }

            if (it.bmp) {
                int dw, dh;
                ThumbFitSize(it.imageWidth, it.imageHeight, boxW_, boxH_, dw, dh);
                const int dx = box.left + (boxW_ - dw) / 2, dy = box.top + (boxH_ - dh) / 2;
                HGDIOBJ old = SelectObject(memDC, it.bmp->handle());
                if (dw == it.bmp->width() && dh == it.bmp->height()) {
                    BitBlt(dc, dx, dy, dw, dh, memDC, 0, 0, SRCCOPY);
                } else {
                    SetStretchBltMode(dc, HALFTONE);
                    SetBrushOrgEx(dc, 0, 0, nullptr);
                    StretchBlt(dc, dx, dy, dw, dh, memDC, 0, 0, it.bmp->width(), it.bmp->height(), SRCCOPY);
                }
                SelectObject(memDC, old);
            }
            wchar_t buf[96];
            std::swprintf(buf, std::size(buf), L"%d × %d   ·   ", it.imageWidth, it.imageHeight);
            detail = buf + Widen(it.format);
        } else if (it.state == GridItem::State::Failed) {
            SelectObject(dc, fonts.detail);
            Text(dc, box, L"Can't preview this file", kFaint, DT_CENTER | DT_VCENTER);
            detail = L"Not supported";
        } else {
            SelectObject(dc, fonts.detail);
            Text(dc, box, L"Loading…", kFaint, DT_CENTER | DT_VCENTER);
        }

        RECT nameR = {box.left, box.bottom + Scale(6), box.right, box.bottom + Scale(26)};
        SelectObject(dc, fonts.name);
        Text(dc, nameR, it.name, kName, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        if (!detail.empty()) {
            RECT detR = {box.left, nameR.bottom, box.right, nameR.bottom + Scale(18)};
            SelectObject(dc, fonts.detail);
            Text(dc, detR, detail, kDetail, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    // Keep 8-bit copies only near the screen; the compact HDR thumbnails stay.
    const int span = std::max(cols_ * 2, (last - first + 1) * 2);
    for (int i = 0; i < static_cast<int>(items.size()); ++i)
        if ((i < first - span || i > last + span) && items[static_cast<size_t>(i)].bmp)
            items[static_cast<size_t>(i)].bmp.reset();

    if (maxScroll_ > 0) FillRound(dc, ScrollThumbRect(), scrollHot ? kScrollHot : kScroll, Scale(3));

    DeleteDC(memDC);
    RestoreDC(dc, saved);
}
