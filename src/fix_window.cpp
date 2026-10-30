// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fix_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <memory>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"EXRayFixWindow";

const COLORREF kBg        = RGB(0x1e, 0x1e, 0x1e);
const COLORREF kTitle     = RGB(0xf0, 0xf0, 0xf0);
const COLORREF kDim       = RGB(0x8a, 0x8a, 0x8a);
const COLORREF kOff       = RGB(0x5a, 0x5a, 0x5a);
const COLORREF kHeader    = RGB(0xe6, 0xe6, 0xe6);
const COLORREF kRule      = RGB(0x34, 0x34, 0x34);
const COLORREF kBody      = RGB(0xbc, 0xbc, 0xbc);
const COLORREF kButton    = RGB(0x33, 0x33, 0x33);
const COLORREF kButtonHot = RGB(0x40, 0x40, 0x40);
const COLORREF kAccent    = RGB(0x3a, 0x7b, 0xc8);
const COLORREF kAccentHot = RGB(0x4a, 0x8b, 0xd8);
const COLORREF kFooter    = RGB(0x25, 0x25, 0x25);
const COLORREF kOk        = RGB(0x5c, 0xb8, 0x6a);
const COLORREF kWarn      = RGB(0xe0, 0xa8, 0x3c);
const COLORREF kBad       = RGB(0xe0, 0x4a, 0x5a);

constexpr SkyType kSkies[] = {SkyType::Clear, SkyType::LightHaze, SkyType::Hazy};
constexpr float kSizes[] = {kRealSunSizeDeg, 1.0f, 2.0f, 4.0f};

enum class Mark { Ok, Warn, Note, Bad };

struct Finding { Mark mark; std::wstring text; RECT r{}; };

struct Check {
    bool* value = nullptr;
    bool enabled = false;
    std::wstring label, note;
    RECT row{}, box{};
};

// A row of segmented buttons under "Rebuild the clipped sun".
struct Choice {
    std::wstring label;
    std::vector<std::wstring> items;
    std::vector<int> widths;        // at 100% scaling
    RECT labelR{};
    std::vector<RECT> segs;
};
enum { kChoiceSky = 0, kChoiceSize = 1, kChoiceColor = 2 };

enum Hot { kNone = -1, kApply = 100, kCancel = 101, kSunCheck = 102, kSegBase = 200, kCheckBase = 300 };
int SegId(int choice, int item) { return kSegBase + choice * 16 + item; }

struct State {
    std::wstring fileName;
    HdriReport report;
    HdriFixOptions opts;
    HWND parent = nullptr;
    bool loaded = false;          // a report has been shown
    UINT dpi = 96;
    HFONT titleFont = nullptr, headerFont = nullptr, bodyFont = nullptr, boldFont = nullptr, smallFont = nullptr;
    int margin = 0, width = 0, footerH = 0, height = 0;   // height = content height (scrolls)
    int scroll = 0, viewH = 0;    // the content scrolls above the footer
    std::wstring subtitle;
    int subtitleH = 0;
    RECT foundHeader{}, fixHeader{};
    std::vector<Finding> findings;
    std::vector<Check> checks;    // 0 = sun, 1 = broken, 2 = hot
    std::vector<Choice> choices;  // sky, size, colour
    RECT peakText{}, sunNote{};
    bool showSun = false;         // the sun choices are shown
    bool canSunCheck = false;     // a sun was located
    RECT apply{}, cancel{}, sunCheck{};
    std::wstring footNote;
    RECT footRect{};
    int hot = kNone, pressed = kNone;

    int S(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
    bool CanApply() const {
        for (const Check& c : checks) if (c.enabled && *c.value) return true;
        return false;
    }
    int Selected(int choice) const {
        if (choice == kChoiceSky) {
            for (int k = 0; k < 3; ++k) if (opts.sky == kSkies[k]) return k;
            return 0;
        }
        if (choice == kChoiceSize) {
            for (int k = 0; k < 4; ++k) if (std::fabs(opts.sunSizeDeg - kSizes[k]) < 0.01f) return k;
            return 0;
        }
        return opts.sunColor == SunColorMode::Natural ? 1 : 0;
    }
    void Select(int choice, int k) {
        if (choice == kChoiceSky) opts.sky = kSkies[std::clamp(k, 0, 2)];
        else if (choice == kChoiceSize) opts.sunSizeDeg = kSizes[std::clamp(k, 0, 3)];
        else opts.sunColor = k == 1 ? SunColorMode::Natural : SunColorMode::Glow;
    }
    void FreeFonts() {
        for (HFONT* f : {&titleFont, &headerFont, &bodyFont, &boldFont, &smallFont}) { if (*f) DeleteObject(*f); *f = nullptr; }
    }
    ~State() { FreeFonts(); }
};

HFONT MakeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

// 324000 -> "324,000"; small values keep a decimal or two.
std::wstring Num(double v) {
    wchar_t buf[64];
    if (v < 10.0) { std::swprintf(buf, std::size(buf), L"%.2f", v); return buf; }
    if (v < 1000.0) { std::swprintf(buf, std::size(buf), L"%.1f", v); return buf; }
    std::swprintf(buf, std::size(buf), L"%.0f", v);
    std::wstring s = buf, out;
    const int len = static_cast<int>(s.size());
    for (int i = 0; i < len; ++i) {
        out += s[static_cast<size_t>(i)];
        const int left = len - 1 - i;
        if (left > 0 && left % 3 == 0) out += L',';
    }
    return out;
}

// Whole numbers with thousands separators: 7 -> "7", 12345 -> "12,345".
std::wstring Count(size_t n) {
    const std::wstring s = std::to_wstring(n);
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        const size_t left = s.size() - 1 - i;
        if (left > 0 && left % 3 == 0) out += L',';
    }
    return out;
}

// "less than 1 %", "3 %", "40 %".
std::wstring Percent(float share) {
    const float p = share * 100.0f;
    if (p < 1.0f) return L"less than 1 %";
    wchar_t buf[32];
    std::swprintf(buf, std::size(buf), L"about %.0f %%", static_cast<double>(p));
    return buf;
}

void BuildText(State& st) {
    const HdriReport& r = st.report;
    wchar_t buf[640];
    std::swprintf(buf, std::size(buf), L"%d × %d   ·   brightest value %ls   ·   %.1f stops of range",
                  r.width, r.height, Num(r.maxValue).c_str(), static_cast<double>(r.rangeStops));
    st.subtitle = st.fileName + L"   ·   " + buf;

    auto add = [&](Mark m, std::wstring t) { st.findings.push_back({m, std::move(t), {}}); };

    if (r.notHdr) {
        add(Mark::Bad, L"Not real HDR: nothing is brighter than 1.0. This is an ordinary picture saved in an HDR "
                       L"format — it can't light a scene with real contrast, and that can't be fixed here.");
    }
    switch (r.sun) {
        case HdriReport::Sun::Rebuildable:
            if (r.wideGlow) {
                std::swprintf(buf, std::size(buf),
                              L"Clipped glow: flat at %ls over %ls pixels (%.1f° across), %.0f° above the horizon. That is "
                              L"much wider than a sun — usually a hazy or sunset sun whose light is spread out. Rebuilding "
                              L"a sharp sun here would add far too much light, so it isn't ticked.",
                              Num(r.clipLevel).c_str(), Count(r.clippedPixels).c_str(), static_cast<double>(r.sunSizeDeg),
                              static_cast<double>(r.sunElevation));
                add(Mark::Note, buf);
                break;
            }
            std::swprintf(buf, std::size(buf),
                          L"Clipped sun (or light): flat at %ls over %ls pixels (%.1f° across), %.0f° above the horizon. "
                          L"It holds %ls of the light a clear-sky sun there would give — shadows will be soft and "
                          L"highlights dull.",
                          Num(r.clipLevel).c_str(), Count(r.clippedPixels).c_str(), static_cast<double>(r.sunSizeDeg),
                          static_cast<double>(r.sunElevation), Percent(r.lightShare).c_str());
            add(Mark::Warn, buf);
            break;
        case HdriReport::Sun::LooksOk:
            add(Mark::Ok, L"Sun not clipped: its brightest part is already about as bright as a real sun.");
            break;
        case HdriReport::Sun::TooLarge:
            add(Mark::Warn, L"A large area is clipped (wider than a sun — a window, a lamp shade or overexposed sky). "
                            L"Its real brightness is lost and can't be rebuilt.");
            break;
        case HdriReport::Sun::BelowHorizon:
            add(Mark::Note, L"Clipped areas only below the horizon (reflections or lamps) — left as they are.");
            break;
        case HdriReport::Sun::NotPanorama:
            add(Mark::Note, L"Clipped areas found, but the image isn't a 2:1 panorama, so a sun can't be placed in it.");
            break;
        case HdriReport::Sun::None:
            if (!r.notHdr) add(Mark::Ok, L"No clipping: the brightest values look real.");
            break;
    }
    if (r.nanPixels || r.infPixels || r.negativePixels) {
        std::swprintf(buf, std::size(buf), L"Broken pixels: %ls NaN, %ls infinite, %ls negative. They show up as black or "
                                           L"white spots and fireflies in renders.",
                      Count(r.nanPixels).c_str(), Count(r.infPixels).c_str(), Count(r.negativePixels).c_str());
        add(Mark::Warn, buf);
    } else {
        add(Mark::Ok, L"No broken pixels (NaN, infinite or negative values).");
    }
    if (HdriManyHotPixels(r)) {
        std::swprintf(buf, std::size(buf), L"%ls tiny bright points — probably stars or small lights rather than "
                                           L"faults, so not ticked below.", Count(r.hotPixels).c_str());
        add(Mark::Note, buf);
    } else if (r.hotPixels) {
        std::swprintf(buf, std::size(buf), L"Hot pixels: %ls single pixel%ls far brighter than everything around.",
                      Count(r.hotPixels).c_str(), r.hotPixels == 1 ? L"" : L"s");
        add(Mark::Warn, buf);
    } else {
        add(Mark::Ok, L"No hot pixels.");
    }

    st.checks.resize(3);
    st.checks[0] = {&st.opts.rebuildSun, r.sun == HdriReport::Sun::Rebuildable, L"Rebuild the clipped sun",
                    L"A sun disc with a brightness estimated from the sky around it: sharp shadows and bright highlights again.", {}, {}};
    st.checks[1] = {&st.opts.repairBroken, r.nanPixels || r.infPixels || r.negativePixels, L"Repair broken pixels",
                    L"NaN and infinite values take their neighbours' average; negative values become 0.", {}, {}};
    st.checks[2] = {&st.opts.removeHotPixels, r.hotPixels > 0, L"Remove hot pixels",
                    L"Each one takes the median of its neighbours.", {}, {}};
    for (Check& c : st.checks) if (!c.enabled) *c.value = false;
    st.showSun = st.checks[0].enabled;
    st.canSunCheck = r.panorama && r.sunSizeDeg > 0.0f;

    st.choices = {
        {L"Sky", {L"Clear", L"Light haze", L"Hazy"}, {64, 92, 64}, {}, {}},
        {L"Size", {L"Real 0.53°", L"1°", L"2°", L"4°"}, {92, 44, 44, 44}, {}, {}},
        {L"Colour", {L"From the glow", L"Natural"}, {120, 84}, {}, {}},
    };

    st.footNote = r.AnythingToFix() ? L"Fixes show on the image first; B shows before / after and Ctrl+S saves a copy. "
                                      L"The original file is never changed."
                                    : L"Nothing here needs fixing.";
}

int WrappedH(HDC dc, HFONT f, const std::wstring& t, int width) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{0, 0, std::max(1, width), 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

const wchar_t kSunNote[] = L"Pick the sky you see around the sun: deep blue and a crisp sun = Clear; pale or milky = "
                           L"Light haze or Hazy. Haze moves light from the sun to the sky, so the same sky means a weaker "
                           L"sun. Natural colour = daylight for the sun's height. A bigger sun gives softer shadows with the same total "
                           L"light. A lamp indoors: untick this.";

// Lays the panel out for its current width. Choice rows put their label above
// the buttons, and the buttons shrink to fit a narrow panel.
void Layout(HWND hwnd, State& st) {
    HDC dc = GetDC(hwnd);
    RECT cr;
    GetClientRect(hwnd, &cr);
    st.margin = st.S(18);
    st.width = std::max<int>(cr.right, st.S(200));
    st.footerH = st.S(58);
    const int inner = st.width - 2 * st.margin;
    int y = st.margin + st.S(30);
    st.subtitleH = WrappedH(dc, st.bodyFont, st.subtitle, inner);
    y += st.subtitleH + st.S(12);

    st.foundHeader = {st.margin, y, st.width - st.margin, y + st.S(30)};
    y += st.S(36);
    const int markW = st.S(22);
    for (Finding& f : st.findings) {
        const int th = WrappedH(dc, st.bodyFont, f.text, inner - markW);
        f.r = {st.margin, y, st.width - st.margin, y + th};
        y += th + st.S(10);
    }

    y += st.S(10);
    st.fixHeader = {st.margin, y, st.width - st.margin, y + st.S(30)};
    y += st.S(38);
    const int boxW = st.S(18), textX = st.margin + boxW + st.S(12);
    const int avail = st.width - st.margin - textX;
    for (size_t i = 0; i < st.checks.size(); ++i) {
        Check& c = st.checks[i];
        const int top = y;
        c.box = {st.margin, y + st.S(1), st.margin + boxW, y + st.S(1) + boxW};
        y += st.S(22);
        y += WrappedH(dc, st.smallFont, c.note, avail) + st.S(4);
        if (i == 0 && st.showSun) {
            y += st.S(6);
            for (size_t g = 0; g < st.choices.size(); ++g) {
                Choice& ch = st.choices[g];
                ch.labelR = {textX, y, st.width - st.margin, y + st.S(20)};
                y += st.S(22);
                int total = 0;
                for (int w : ch.widths) total += st.S(w);
                const double k = total > avail ? static_cast<double>(avail) / total : 1.0;
                int x = textX;
                ch.segs.clear();
                for (size_t n = 0; n < ch.widths.size(); ++n) {
                    int w = static_cast<int>(st.S(ch.widths[n]) * k);
                    if (n + 1 == ch.widths.size() && k < 1.0) w = textX + avail - x;   // fill exactly
                    ch.segs.push_back(RECT{x, y, x + w, y + st.S(28)});
                    x += w;
                }
                y += st.S(34);
                if (g == kChoiceSky) {
                    st.peakText = {textX, y - st.S(4), st.width - st.margin, y + st.S(18)};
                    y += st.S(24);
                }
            }
            const int nh = WrappedH(dc, st.smallFont, kSunNote, avail);
            st.sunNote = {textX, y, st.width - st.margin, y + nh};
            y += nh;
        }
        c.row = {st.margin, top, st.width - st.margin, y};
        y += st.S(14);
    }
    const int fh = WrappedH(dc, st.smallFont, st.footNote, inner);
    st.footRect = {st.margin, y, st.width - st.margin, y + fh};
    y += fh + st.margin;
    st.height = y;
    st.viewH = std::max<int>(0, cr.bottom - st.footerH);
    st.scroll = std::clamp(st.scroll, 0, std::max(0, st.height - st.viewH));
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

void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& t, RECT r, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX);
}

void Header(HDC dc, const State& st, const RECT& r, const wchar_t* t) {
    RECT tr = r;
    tr.bottom -= st.S(6);
    Text(dc, st.headerFont, kHeader, t, tr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    RECT rule{r.left, r.bottom - st.S(3), r.right, r.bottom - st.S(3) + std::max(1, st.S(1))};
    HBRUSH rb = CreateSolidBrush(kRule);
    FillRect(dc, &rule, rb);
    DeleteObject(rb);
}

void Button(HDC dc, const State& st, const RECT& r, const wchar_t* label, bool primary, bool enabled, bool hot) {
    const COLORREF c = primary ? (!enabled ? kButton : hot ? kAccentHot : kAccent) : (hot ? kButtonHot : kButton);
    FillRound(dc, r, c, c, st.S(5));
    Text(dc, st.bodyFont, !enabled ? kOff : primary ? RGB(0xff, 0xff, 0xff) : kHeader, label, r,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void Paint(HWND hwnd, State& st) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) { EndPaint(hwnd, &ps); return; }
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    RECT edge{0, 0, 1, cr.bottom};
    HBRUSH eb = CreateSolidBrush(kRule);
    FillRect(dc, &edge, eb);
    DeleteObject(eb);

    if (st.loaded) {
        // The content scrolls; the footer with the buttons stays put.
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, 0, 0, cr.right, st.viewH);
        SetViewportOrgEx(dc, 0, -st.scroll, nullptr);

        int y = st.margin;
        Text(dc, st.titleFont, kTitle, L"Fix HDRI", RECT{st.margin, y, cr.right - st.margin, y + st.S(30)},
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += st.S(30);
        Text(dc, st.bodyFont, kDim, st.subtitle, RECT{st.margin, y, cr.right - st.margin, y + st.subtitleH}, DT_LEFT | DT_WORDBREAK);

        Header(dc, st, st.foundHeader, L"What was found");
        for (const Finding& f : st.findings) {
            const COLORREF mc = f.mark == Mark::Ok ? kOk : f.mark == Mark::Warn ? kWarn : f.mark == Mark::Bad ? kBad : kDim;
            const int d = st.S(8), cy = f.r.top + st.S(9);
            FillRound(dc, RECT{f.r.left + st.S(2), cy - d / 2, f.r.left + st.S(2) + d, cy + d / 2}, mc, mc, d / 2);
            Text(dc, st.bodyFont, kBody, f.text, RECT{f.r.left + st.S(22), f.r.top, f.r.right, f.r.bottom}, DT_LEFT | DT_WORDBREAK);
        }

        Header(dc, st, st.fixHeader, L"Fixes");
        for (size_t i = 0; i < st.checks.size(); ++i) {
            const Check& c = st.checks[i];
            const bool on = c.enabled && *c.value;
            const bool hot = c.enabled && st.hot == kCheckBase + static_cast<int>(i);
            const COLORREF boxFill = on ? (hot ? kAccentHot : kAccent) : (hot ? kButtonHot : kButton);
            FillRound(dc, c.box, c.enabled ? boxFill : kBg, c.enabled ? (on ? boxFill : RGB(0x5a, 0x5a, 0x5a)) : kRule, st.S(3));
            if (on) {   // tick mark
                HPEN pen = CreatePen(PS_SOLID, std::max(2, st.S(2)), RGB(0xff, 0xff, 0xff));
                HGDIOBJ op = SelectObject(dc, pen);
                const int bx = c.box.left, by = c.box.top, sz = c.box.right - c.box.left;
                POINT pts[3] = {{bx + sz * 22 / 100, by + sz * 52 / 100}, {bx + sz * 42 / 100, by + sz * 72 / 100},
                                {bx + sz * 78 / 100, by + sz * 30 / 100}};
                Polyline(dc, pts, 3);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            const int tx = c.box.right + st.S(12);
            Text(dc, st.boldFont, c.enabled ? kHeader : kOff, c.label + (c.enabled ? L"" : L"  (nothing to fix)"),
                 RECT{tx, c.row.top, c.row.right, c.row.top + st.S(22)}, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, st.smallFont, c.enabled ? kDim : kOff, c.note, RECT{tx, c.row.top + st.S(22), c.row.right, c.row.bottom},
                 DT_LEFT | DT_WORDBREAK);

            if (i == 0 && st.showSun) {
                const bool en = st.opts.rebuildSun;
                for (size_t g = 0; g < st.choices.size(); ++g) {
                    const Choice& ch = st.choices[g];
                    Text(dc, st.smallFont, en ? kDim : kOff, ch.label, ch.labelR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    RECT group{ch.segs.front().left, ch.segs.front().top, ch.segs.back().right, ch.segs.front().bottom};
                    FillRound(dc, group, kButton, kButton, st.S(5));
                    const int sel = st.Selected(static_cast<int>(g));
                    for (size_t k = 0; k < ch.segs.size(); ++k) {
                        RECT r = ch.segs[k];
                        InflateRect(&r, -std::max(2, st.S(2)), -std::max(2, st.S(2)));
                        const bool h = en && st.hot == SegId(static_cast<int>(g), static_cast<int>(k));
                        const COLORREF on2 = en ? (h ? kAccentHot : kAccent) : RGB(0x4a, 0x4a, 0x4a);
                        if (static_cast<int>(k) == sel) FillRound(dc, r, on2, on2, st.S(4));
                        else if (h) FillRound(dc, r, kButtonHot, kButtonHot, st.S(4));
                        Text(dc, st.bodyFont, en ? kHeader : kOff, ch.items[k], ch.segs[k],
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }
                wchar_t buf[96];
                std::swprintf(buf, std::size(buf), L"Sun brightness  →  about %ls",
                              Num(SunPeakAt(st.report, SkyEV(st.opts.sky) + st.opts.sunStrengthEV, st.opts.sunSizeDeg)).c_str());
                Text(dc, st.bodyFont, en ? kBody : kOff, buf, st.peakText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                Text(dc, st.smallFont, en ? kDim : kOff, kSunNote, st.sunNote, DT_LEFT | DT_WORDBREAK);
            }
        }
        Text(dc, st.smallFont, kDim, st.footNote, st.footRect, DT_LEFT | DT_WORDBREAK);
        RestoreDC(dc, saved);

        // Scroll indicator.
        if (st.height > st.viewH && st.viewH > 0) {
            const int thumbH = std::max(st.S(24), st.viewH * st.viewH / st.height);
            const int maxScroll = st.height - st.viewH;
            const int ty = (st.viewH - thumbH) * st.scroll / std::max(1, maxScroll);
            FillRound(dc, RECT{cr.right - st.S(5), ty + st.S(2), cr.right - st.S(2), ty + thumbH - st.S(2)}, kButtonHot, kButtonHot, st.S(1));
        }

        // Footer: Sun check on the left, Close and Apply on the right.
        const int bottom = cr.bottom - st.footerH;
        RECT fr{1, bottom, cr.right, cr.bottom};
        HBRUSH fb = CreateSolidBrush(kFooter);
        FillRect(dc, &fr, fb);
        DeleteObject(fb);
        const int bh = st.S(32), by = bottom + (st.footerH - bh) / 2, m = st.S(14);
        const bool anything = st.report.AnythingToFix();
        int right = cr.right - m;
        st.apply = RECT{};
        if (anything) { st.apply = {right - st.S(92), by, right, by + bh}; right = st.apply.left - st.S(8); }
        st.cancel = {right - st.S(72), by, right, by + bh};
        st.sunCheck = RECT{};
        if (st.canSunCheck) st.sunCheck = {m, by, std::min<int>(m + st.S(96), st.cancel.left - st.S(8)), by + bh};
        if (anything) Button(dc, st, st.apply, L"Apply", true, st.CanApply(), st.hot == kApply);
        Button(dc, st, st.cancel, L"Close", false, true, st.hot == kCancel);
        if (st.canSunCheck) Button(dc, st, st.sunCheck, L"Sun check", false, true, st.hot == kSunCheck);
    }

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

int HitTest(const State& st, POINT p) {
    if (!st.loaded) return kNone;
    if (p.y >= st.viewH) {   // the footer does not scroll
        if (st.report.AnythingToFix() && PtInRect(&st.apply, p)) return kApply;
        if (PtInRect(&st.cancel, p)) return kCancel;
        if (st.canSunCheck && PtInRect(&st.sunCheck, p)) return kSunCheck;
        return kNone;
    }
    p.y += st.scroll;
    if (st.showSun && st.opts.rebuildSun)
        for (size_t g = 0; g < st.choices.size(); ++g)
            for (size_t k = 0; k < st.choices[g].segs.size(); ++k)
                if (PtInRect(&st.choices[g].segs[k], p)) return SegId(static_cast<int>(g), static_cast<int>(k));
    for (size_t i = 0; i < st.checks.size(); ++i) {
        if (!st.checks[i].enabled) continue;
        RECT row = st.checks[i].row;
        if (i == 0 && st.showSun) row.bottom = st.choices[0].labelR.top;   // the choice rows have their own targets
        if (PtInRect(&row, p)) return kCheckBase + static_cast<int>(i);
    }
    return kNone;
}

void Notify(State& st, FixWindowResult r) {
    if (st.parent) PostMessageW(st.parent, WM_APP_FIXPANEL, static_cast<WPARAM>(r), 0);
}

void Click(HWND hwnd, State& st, int target) {
    if (target == kApply) {
        if (!st.CanApply()) { MessageBeep(MB_OK); return; }
        Notify(st, FixWindowResult::Apply);
    } else if (target == kCancel) {
        Notify(st, FixWindowResult::Cancel);
    } else if (target == kSunCheck) {
        Notify(st, FixWindowResult::SunCheck);
    } else if (target >= kSegBase && target < kCheckBase) {
        st.Select((target - kSegBase) / 16, (target - kSegBase) % 16);
    } else if (target >= kCheckBase) {
        Check& c = st.checks[static_cast<size_t>(target - kCheckBase)];
        *c.value = !*c.value;
        Layout(hwnd, st);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MakeFonts(State& st) {
    st.FreeFonts();
    st.titleFont = MakeFont(st.S(20), FW_SEMIBOLD);
    st.headerFont = MakeFont(st.S(15), FW_SEMIBOLD);
    st.bodyFont = MakeFont(st.S(13), FW_NORMAL);
    st.boldFont = MakeFont(st.S(13), FW_SEMIBOLD);
    st.smallFont = MakeFont(st.S(12), FW_NORMAL);
}

LRESULT CALLBACK FixProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_NCDESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (st && st->loaded) Layout(hwnd, *st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            if (st) { Paint(hwnd, *st); return 0; }
            break;
        case WM_MOUSEWHEEL:
            if (st && st->height > st->viewH) {
                const int step = -GET_WHEEL_DELTA_WPARAM(wp) * st->S(48) / WHEEL_DELTA;
                const int s = std::clamp(st->scroll + step, 0, st->height - st->viewH);
                if (s != st->scroll) { st->scroll = s; st->hot = kNone; InvalidateRect(hwnd, nullptr, FALSE); }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (st) {
                const int h = HitTest(*st, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                if (h != st->hot) { st->hot = h; InvalidateRect(hwnd, nullptr, FALSE); }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (st && st->hot != kNone) { st->hot = kNone; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_SETCURSOR:
            if (st && LOWORD(lp) == HTCLIENT && st->hot != kNone) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (st) st->pressed = HitTest(*st, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP:
            if (st) {
                const int h = HitTest(*st, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
                const int was = st->pressed;
                st->pressed = kNone;
                if (h != kNone && h == was) Click(hwnd, *st, h);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND CreateFixPanel(HWND parent) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = FixProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* st = new State();
    st->parent = parent;
    st->dpi = GetDpiForWindow(parent);
    HWND hwnd = CreateWindowExW(0, kClass, L"Fix HDRI", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, nullptr, inst, st);
    if (!hwnd) delete st;
    return hwnd;
}

void FixPanelShow(HWND panel, const std::wstring& fileName, const HdriReport& report, const HdriFixOptions& opts) {
    State* st = reinterpret_cast<State*>(GetWindowLongPtrW(panel, GWLP_USERDATA));
    if (!st) return;
    st->dpi = GetDpiForWindow(st->parent);
    MakeFonts(*st);
    st->fileName = fileName;
    st->report = report;
    st->opts = opts;
    st->findings.clear();
    st->checks.clear();
    st->choices.clear();
    st->scroll = 0;
    st->hot = st->pressed = kNone;
    BuildText(*st);
    st->loaded = true;
    Layout(panel, *st);
    InvalidateRect(panel, nullptr, FALSE);
}

void FixPanelDpiChanged(HWND panel) {
    State* st = reinterpret_cast<State*>(GetWindowLongPtrW(panel, GWLP_USERDATA));
    if (!st || !st->loaded) return;
    st->dpi = GetDpiForWindow(st->parent);
    MakeFonts(*st);
    Layout(panel, *st);
    InvalidateRect(panel, nullptr, FALSE);
}

HdriFixOptions FixPanelOptions(HWND panel) {
    State* st = reinterpret_cast<State*>(GetWindowLongPtrW(panel, GWLP_USERDATA));
    return st ? st->opts : HdriFixOptions{};
}
