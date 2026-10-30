// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "info_window.h"

#include <windowsx.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

constexpr wchar_t kClass[] = L"EXRayInfoWindow";

const COLORREF kBg        = RGB(0x1e, 0x1e, 0x1e);
const COLORREF kTitle     = RGB(0xf0, 0xf0, 0xf0);
const COLORREF kSubtitle  = RGB(0x8a, 0x8a, 0x8a);
const COLORREF kHeader    = RGB(0xe6, 0xe6, 0xe6);
const COLORREF kRule      = RGB(0x34, 0x34, 0x34);
const COLORREF kBody      = RGB(0xbc, 0xbc, 0xbc);
const COLORREF kChipBg    = RGB(0x33, 0x33, 0x33);
const COLORREF kChipEdge  = RGB(0x46, 0x46, 0x46);
const COLORREF kChipText  = RGB(0xe8, 0xe8, 0xe8);
const COLORREF kAccent    = RGB(0x3a, 0x7b, 0xc8);
const COLORREF kAccentHot = RGB(0x4a, 0x8b, 0xd8);
const COLORREF kFooter    = RGB(0x25, 0x25, 0x25);

// One laid-out row (positions relative to the top of the content).
struct Row {
    const InfoItem* item = nullptr;
    int column = 0;
    int y = 0, h = 0;
};

struct State {
    InfoPage page;
    UINT dpi = 96;
    HFONT titleFont = nullptr, headerFont = nullptr, bodyFont = nullptr, chipFont = nullptr;
    std::vector<Row> rows;
    std::vector<int> keyColumn;   // width of the key column, per page column
    int colW = 0, colGap = 0, margin = 0, headerH = 0, footerH = 0;
    int contentH = 0, viewH = 0, scroll = 0;
    RECT closeBtn{};
    bool closeHot = false, closePressed = false;
    bool done = false;
    HFONT symbolFont = nullptr, linkFont = nullptr, subFont = nullptr;
    HBITMAP logo = nullptr;       // premultiplied BGRA at the size it is drawn
    int logoW = 0, logoH = 0;
    // Clickable texts (e-mail, "Licenses"), in client coordinates; rebuilt while painting.
    struct Link { RECT r; std::wstring url; const InfoItem* item; };
    std::vector<Link> links;
    int linkHot = -1, linkPressed = -1;

    int S(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
    ~State() {
        for (HFONT f : {titleFont, headerFont, bodyFont, chipFont, symbolFont, linkFont, subFont}) if (f) DeleteObject(f);
        if (logo) DeleteObject(logo);
    }
};

HFONT MakeFont(int px, int weight, const wchar_t* face = L"Segoe UI", bool underline = false) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, underline ? TRUE : FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
}

// The logo is stored in the .exe (resource 2) as: width, height, then
// premultiplied BGRA pixels. It is shrunk here (area average) to `size` pixels tall.
HBITMAP LoadLogo(int size, int& outW, int& outH) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return nullptr;
    HGLOBAL h = LoadResource(mod, res);
    const auto* p = static_cast<const uint8_t*>(h ? LockResource(h) : nullptr);
    const DWORD bytes = SizeofResource(mod, res);
    if (!p || bytes < 8) return nullptr;
    uint32_t sw, sh;
    std::memcpy(&sw, p, 4);
    std::memcpy(&sh, p + 4, 4);
    if (sw == 0 || sh == 0 || bytes < 8 + static_cast<size_t>(sw) * sh * 4) return nullptr;
    const uint8_t* src = p + 8;

    const int dh = std::max(1, std::min(size, static_cast<int>(sh)));
    const int dw = std::max(1, static_cast<int>(static_cast<long long>(sw) * dh / sh));
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    auto* dst = static_cast<uint8_t*>(bits);
    for (int y = 0; y < dh; ++y) {
        const int y0 = static_cast<int>(static_cast<long long>(y) * sh / dh);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int x0 = static_cast<int>(static_cast<long long>(x) * sw / dw);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * sw / dw));
            unsigned sum[4] = {0, 0, 0, 0};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c) sum[c] += src[(static_cast<size_t>(yy) * sw + xx) * 4 + c];
            const unsigned n = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < 4; ++c) dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    GdiFlush();
    outW = dw;
    outH = dh;
    return bmp;
}

int LogoSize(const State& st) { return st.S(84); }

std::vector<std::wstring> SplitChips(const std::wstring& keys) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t bar = keys.find(L'|', start);
        out.push_back(keys.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
        if (bar == std::wstring::npos) break;
        start = bar + 1;
    }
    return out;
}

int TextW(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.right - r.left;
}

int WrappedH(HDC dc, HFONT f, const std::wstring& t, int width) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, std::max(1, width), 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

int ChipPad(const State& st) { return st.S(9); }
int ChipGap(const State& st) { return st.S(4); }
int ChipH(const State& st) { return st.S(22); }

int ChipsWidth(HDC dc, const State& st, const std::wstring& keys) {
    int w = 0;
    auto chips = SplitChips(keys);
    for (size_t i = 0; i < chips.size(); ++i) {
        w += TextW(dc, st.chipFont, chips[i]) + 2 * ChipPad(st);
        if (i + 1 < chips.size()) w += ChipGap(st);
    }
    return w;
}

void Layout(HWND hwnd, State& st) {
    HDC dc = GetDC(hwnd);
    const int textGap = st.S(14);
    st.colW = st.S(st.page.columnWidth);
    st.colGap = st.S(40);
    st.margin = st.S(28);
    st.footerH = st.S(56);

    // Title block.
    st.headerH = st.margin + st.S(30);
    if (!st.page.subtitle.empty()) st.headerH += st.S(22);
    st.headerH += st.S(14);

    // Each column's key width is measured, so its descriptions all start at
    // the same x, whatever the keys.
    st.keyColumn.assign(st.page.columns.size(), 0);
    for (size_t c = 0; c < st.page.columns.size(); ++c)
        for (const InfoItem& it : st.page.columns[c])
            if (it.kind == InfoItem::Kind::Keys) st.keyColumn[c] = std::max(st.keyColumn[c], ChipsWidth(dc, st, it.keys));

    st.rows.clear();
    st.contentH = 0;
    for (size_t c = 0; c < st.page.columns.size(); ++c) {
        int y = 0;
        bool first = true;
        for (const InfoItem& it : st.page.columns[c]) {
            Row r;
            r.item = &it;
            r.column = static_cast<int>(c);
            switch (it.kind) {
                case InfoItem::Kind::Header:
                    if (!first) y += st.S(16);
                    r.y = y;
                    r.h = st.S(30);
                    break;
                case InfoItem::Kind::Keys: {
                    const int descW = st.colW - st.keyColumn[c] - textGap;
                    const int th = WrappedH(dc, st.bodyFont, it.text, descW);
                    r.y = y;
                    r.h = std::max(st.S(28), th + st.S(8));
                    break;
                }
                case InfoItem::Kind::Paragraph:
                    r.y = y;
                    r.h = WrappedH(dc, st.bodyFont, it.text, st.colW) + st.S(10);
                    break;
                case InfoItem::Kind::Bullet:
                    r.y = y;
                    r.h = WrappedH(dc, st.bodyFont, it.text, st.colW - st.S(18)) + st.S(6);
                    break;
                case InfoItem::Kind::Gap:
                    r.y = y;
                    r.h = st.S(10);
                    break;
                case InfoItem::Kind::Subheader:
                    if (!first) y += st.S(6);
                    r.y = y;
                    r.h = st.S(24);
                    break;
                case InfoItem::Kind::Action:
                    r.y = y;
                    r.h = st.S(26);
                    break;
                case InfoItem::Kind::Signature:
                    y += st.S(6);
                    r.y = y;
                    r.h = LogoSize(st) + st.S(12);
                    break;
            }
            y += r.h;
            first = false;
            st.rows.push_back(r);
        }
        st.contentH = std::max(st.contentH, y);
    }
    ReleaseDC(hwnd, dc);
}

void FillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF edge, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

void Paint(HWND hwnd, State& st) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);

    // Title.
    int y = st.margin;
    SelectObject(dc, st.titleFont);
    SetTextColor(dc, kTitle);
    RECT tr{st.margin, y, cr.right - st.margin, y + st.S(30)};
    DrawTextW(dc, st.page.title.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    y += st.S(30);
    if (!st.page.subtitle.empty()) {
        SelectObject(dc, st.bodyFont);
        SetTextColor(dc, kSubtitle);
        RECT sr{st.margin, y, cr.right - st.margin, y + st.S(22)};
        DrawTextW(dc, st.page.subtitle.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Content (scrolls if it doesn't fit).
    const int top = st.headerH;
    const int bottom = cr.bottom - st.footerH;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, top, cr.right, bottom);
    const int textGap = st.S(14);
    st.links.clear();
    for (const Row& r : st.rows) {
        const int x0 = st.margin + r.column * (st.colW + st.colGap);
        const int ry = top + r.y - st.scroll;
        if (ry + r.h < top || ry > bottom) continue;
        const InfoItem& it = *r.item;
        switch (it.kind) {
            case InfoItem::Kind::Header: {
                SelectObject(dc, st.headerFont);
                SetTextColor(dc, kHeader);
                RECT hr{x0, ry, x0 + st.colW, ry + r.h - st.S(6)};
                DrawTextW(dc, it.text.c_str(), -1, &hr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
                RECT rule{x0, ry + r.h - st.S(3), x0 + st.colW, ry + r.h - st.S(3) + std::max(1, st.S(1))};
                HBRUSH rb = CreateSolidBrush(kRule);
                FillRect(dc, &rule, rb);
                DeleteObject(rb);
                break;
            }
            case InfoItem::Kind::Keys: {
                // Chips, vertically centred on the first line of the description.
                const int lineH = st.S(28);
                int cx = x0;
                const int cy = ry + (lineH - ChipH(st)) / 2;
                SelectObject(dc, st.chipFont);
                for (const std::wstring& chip : SplitChips(it.keys)) {
                    const int w = TextW(dc, st.chipFont, chip) + 2 * ChipPad(st);
                    RECT c{cx, cy, cx + w, cy + ChipH(st)};
                    FillRound(dc, c, kChipBg, kChipEdge, st.S(4));
                    SetTextColor(dc, kChipText);
                    DrawTextW(dc, chip.c_str(), -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    cx += w + ChipGap(st);
                }
                SelectObject(dc, st.bodyFont);
                SetTextColor(dc, kBody);
                const int dx = x0 + st.keyColumn[r.column] + textGap;
                const int th = WrappedH(dc, st.bodyFont, it.text, x0 + st.colW - dx);
                // Single-line descriptions sit centred on the chip row; longer ones start level with it.
                const int ty = ry + std::max(0, (lineH - std::min(th, lineH)) / 2);
                RECT dr{dx, ty, x0 + st.colW, ty + th};
                DrawTextW(dc, it.text.c_str(), -1, &dr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                break;
            }
            case InfoItem::Kind::Paragraph: {
                SelectObject(dc, st.bodyFont);
                SetTextColor(dc, kBody);
                RECT pr{x0, ry, x0 + st.colW, ry + r.h};
                DrawTextW(dc, it.text.c_str(), -1, &pr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                break;
            }
            case InfoItem::Kind::Bullet: {
                SelectObject(dc, st.bodyFont);
                SetTextColor(dc, kAccentHot);
                RECT br{x0 + st.S(2), ry, x0 + st.S(16), ry + st.S(20)};
                DrawTextW(dc, L"\u2022", -1, &br, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(dc, kBody);
                RECT tr2{x0 + st.S(18), ry, x0 + st.colW, ry + r.h};
                DrawTextW(dc, it.text.c_str(), -1, &tr2, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                break;
            }
            case InfoItem::Kind::Gap:
                break;
            case InfoItem::Kind::Subheader: {
                SelectObject(dc, st.subFont);
                SetTextColor(dc, kAccentHot);
                RECT sr{x0, ry, x0 + st.colW, ry + r.h};
                DrawTextW(dc, it.text.c_str(), -1, &sr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
                break;
            }
            case InfoItem::Kind::Signature: {
                // Logo on the left, two lines of text beside it.
                int tx = x0;
                if (st.logo) {
                    HDC mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, st.logo);
                    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    GdiAlphaBlend(dc, x0, ry, st.logoW, st.logoH, mem, 0, 0, st.logoW, st.logoH, bf);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                    tx = x0 + st.logoW + st.S(16);
                }
                const int lineH = st.S(22);
                const int ty = ry + (LogoSize(st) - 2 * lineH) / 2;
                // "Made with" + heart + "from Greece": the heart comes from the symbol font, in red.
                const std::wstring a = L"Made with ", b = L" from " + it.text;
                SelectObject(dc, st.bodyFont);
                SetTextColor(dc, kHeader);
                RECT ar{tx, ty, x0 + st.colW, ty + lineH};
                DrawTextW(dc, a.c_str(), -1, &ar, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                int hx = tx + TextW(dc, st.bodyFont, a);
                SelectObject(dc, st.symbolFont);
                SetTextColor(dc, RGB(0xe0, 0x4a, 0x5a));
                RECT hr{hx, ty, x0 + st.colW, ty + lineH};
                const wchar_t heart[] = L"\u2764";
                DrawTextW(dc, heart, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                hx += TextW(dc, st.symbolFont, heart);
                SelectObject(dc, st.bodyFont);
                SetTextColor(dc, kHeader);
                RECT br{hx, ty, x0 + st.colW, ty + lineH};
                DrawTextW(dc, b.c_str(), -1, &br, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                // E-mail link (opens the mail app).
                SelectObject(dc, st.linkFont);
                SetTextColor(dc, st.linkHot == static_cast<int>(st.links.size()) ? RGB(0x8c, 0xbd, 0xf5) : RGB(0x6a, 0xa6, 0xea));
                const int lw = TextW(dc, st.linkFont, it.keys);
                RECT lr{tx, ty + lineH, tx + lw, ty + 2 * lineH};
                DrawTextW(dc, it.keys.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                st.links.push_back({lr, L"mailto:" + it.keys, nullptr});
                break;
            }
            case InfoItem::Kind::Action: {
                SelectObject(dc, st.linkFont);
                SetTextColor(dc, st.linkHot == static_cast<int>(st.links.size()) ? RGB(0x8c, 0xbd, 0xf5) : RGB(0x6a, 0xa6, 0xea));
                const int lw = TextW(dc, st.linkFont, it.text);
                RECT lr{x0, ry, x0 + lw, ry + r.h};
                DrawTextW(dc, it.text.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                st.links.push_back({lr, L"", &it});
                break;
            }
        }
    }
    // A thin scroll indicator when the content is taller than the window.
    if (st.contentH > st.viewH) {
        const int trackH = bottom - top - st.S(8);
        const int thumbH = std::max(st.S(30), trackH * st.viewH / std::max(1, st.contentH));
        const int maxScroll = st.contentH - st.viewH;
        const int ty = top + st.S(4) + (trackH - thumbH) * st.scroll / std::max(1, maxScroll);
        RECT th{cr.right - st.S(8), ty, cr.right - st.S(4), ty + thumbH};
        FillRound(dc, th, RGB(0x4a, 0x4a, 0x4a), RGB(0x4a, 0x4a, 0x4a), st.S(2));
    }
    RestoreDC(dc, saved);

    // Footer with the Close button.
    RECT fr{0, bottom, cr.right, cr.bottom};
    HBRUSH fb = CreateSolidBrush(kFooter);
    FillRect(dc, &fr, fb);
    DeleteObject(fb);
    const int bw = st.S(96), bh = st.S(32);
    st.closeBtn = RECT{cr.right - st.margin - bw, bottom + (st.footerH - bh) / 2, cr.right - st.margin, bottom + (st.footerH - bh) / 2 + bh};
    const COLORREF btn = st.closeHot ? kAccentHot : kAccent;
    FillRound(dc, st.closeBtn, btn, btn, st.S(5));
    SelectObject(dc, st.bodyFont);
    SetTextColor(dc, RGB(0xff, 0xff, 0xff));
    DrawTextW(dc, L"Close", -1, &st.closeBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void ScrollBy(HWND hwnd, State& st, int dy) {
    const int maxScroll = std::max(0, st.contentH - st.viewH);
    const int s = std::clamp(st.scroll + dy, 0, maxScroll);
    if (s != st.scroll) { st.scroll = s; InvalidateRect(hwnd, nullptr, FALSE); }
}

LRESULT CALLBACK InfoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (st) { Paint(hwnd, *st); return 0; }
            break;
        case WM_MOUSEMOVE:
            if (st) {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const bool hot = PtInRect(&st->closeBtn, p) != FALSE;
                if (hot != st->closeHot) { st->closeHot = hot; InvalidateRect(hwnd, &st->closeBtn, FALSE); }
                int lhot = -1;
                for (size_t i = 0; i < st->links.size(); ++i)
                    if (PtInRect(&st->links[i].r, p)) lhot = static_cast<int>(i);
                if (lhot != st->linkHot) { st->linkHot = lhot; InvalidateRect(hwnd, nullptr, FALSE); }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st && (st->closeHot || st->linkHot >= 0)) {
                st->closeHot = false;
                st->linkHot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (st && LOWORD(lp) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                for (const auto& l : st->links)
                    if (PtInRect(&l.r, p)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            }
            break;
        case WM_LBUTTONDOWN:
            if (st) {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                st->closePressed = PtInRect(&st->closeBtn, p) != FALSE;
                st->linkPressed = -1;
                for (size_t i = 0; i < st->links.size(); ++i)
                    if (PtInRect(&st->links[i].r, p)) st->linkPressed = static_cast<int>(i);
            }
            return 0;
        case WM_LBUTTONUP:
            if (st) {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int pressed = st->linkPressed;
                st->linkPressed = -1;
                if (pressed >= 0 && pressed < static_cast<int>(st->links.size()) && PtInRect(&st->links[pressed].r, p)) {
                    const State::Link link = st->links[pressed];
                    if (!link.url.empty())
                        ShellExecuteW(hwnd, L"open", link.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    else if (link.item && link.item->action)
                        link.item->action(hwnd);   // e.g. opens another page on top of this one
                    return 0;
                }
                if (st->closePressed && PtInRect(&st->closeBtn, p)) { st->closePressed = false; DestroyWindow(hwnd); return 0; }
                st->closePressed = false;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (st) ScrollBy(hwnd, *st, -GET_WHEEL_DELTA_WPARAM(wp) * st->S(60) / WHEEL_DELTA);
            return 0;
        case WM_KEYDOWN:
            if (!st) break;
            switch (wp) {
                case VK_ESCAPE: case VK_RETURN: case VK_SPACE: DestroyWindow(hwnd); return 0;
                case VK_DOWN: ScrollBy(hwnd, *st, st->S(40)); return 0;
                case VK_UP: ScrollBy(hwnd, *st, -st->S(40)); return 0;
                case VK_NEXT: ScrollBy(hwnd, *st, st->viewH - st->S(40)); return 0;
                case VK_PRIOR: ScrollBy(hwnd, *st, -(st->viewH - st->S(40))); return 0;
                case VK_HOME: ScrollBy(hwnd, *st, -st->contentH); return 0;
                case VK_END: ScrollBy(hwnd, *st, st->contentH); return 0;
                case VK_F1: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ShowInfoWindow(HWND owner, const InfoPage& page) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InfoProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    auto st = std::make_unique<State>();
    st->page = page;
    st->dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    st->titleFont = MakeFont(st->S(22), FW_SEMIBOLD);
    st->headerFont = MakeFont(st->S(15), FW_SEMIBOLD);
    st->bodyFont = MakeFont(st->S(13), FW_NORMAL);
    st->chipFont = MakeFont(st->S(12), FW_SEMIBOLD);
    st->symbolFont = MakeFont(st->S(14), FW_NORMAL, L"Segoe UI Symbol");
    st->linkFont = MakeFont(st->S(13), FW_NORMAL, L"Segoe UI", true);
    st->subFont = MakeFont(st->S(13), FW_SEMIBOLD);
    for (const auto& col : page.columns)
        for (const InfoItem& it : col)
            if (it.kind == InfoItem::Kind::Signature && !st->logo) st->logo = LoadLogo(LogoSize(*st), st->logoW, st->logoH);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, page.windowTitle.c_str(), style,
                                0, 0, 100, 100, owner, nullptr, inst, st.get());
    if (!hwnd) return;

    BOOL dark = TRUE;
    using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
        if (auto fn = reinterpret_cast<DwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
            fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    // Size to the content, but no taller than the screen allows (then it scrolls).
    Layout(hwnd, *st);
    const int cols = static_cast<int>(page.columns.size());
    int cw = 2 * st->margin + cols * st->colW + std::max(0, cols - 1) * st->colGap;
    int ch = st->headerH + st->contentH + st->S(16) + st->footerH;

    RECT ownerRect{0, 0, 1280, 800};
    if (owner) GetWindowRect(owner, &ownerRect);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONEAREST), &mi);
    RECT frame{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_DLGMODALFRAME, st->dpi);
    const int frameExtraW = (frame.right - frame.left) - cw, frameExtraH = (frame.bottom - frame.top) - ch;
    const int maxW = (mi.rcWork.right - mi.rcWork.left) * 95 / 100, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 90 / 100;
    int ww = std::min(cw + frameExtraW, maxW), wh = std::min(ch + frameExtraH, maxH);
    st->viewH = (wh - frameExtraH) - st->headerH - st->footerH - st->S(16);

    int x = (ownerRect.left + ownerRect.right - ww) / 2, y = (ownerRect.top + ownerRect.bottom - wh) / 2;
    x = std::clamp(x, static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - ww);
    y = std::clamp(y, static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - wh);
    SetWindowPos(hwnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);

    // Modal: the main window waits until this one is closed.
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(hwnd);
    MSG m;
    bool quit = false;
    while (!st->done) {
        const BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) { quit = true; break; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (owner) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (quit) PostQuitMessage(static_cast<int>(m.wParam));
}
