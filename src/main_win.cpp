// EXRay (HDR & EXR viewer) -- Windows viewer
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 2: exposure slider, tonemap choice, zoom & pan, and browsing the
// other .hdr/.exr files in the same folder with the arrow keys.
// Phase 3: folder grid of thumbnails made in the background.
// Phase 4: EXR layers, pixel readout, export to PNG / JPEG.
// v0.5: Cryptomatte, grid multi-select and batch export, thumbnail size, settings file.
// v0.6: export look choice (Clip default), readable depth / normal / vector views.
// v0.8: 360-degree view of equirectangular HDRIs.
// v0.9: Fix HDRI (clipped sun, broken and hot pixels), saved as EXR or HDR.
// v0.10: Studio layout: folder library sidebar, search, inspector, Fix HDRI docked.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "batch_export.h"
#include "display_win.h"
#include "exr_doc.h"
#include "export.h"
#include "fix_window.h"
#include "folder_nav.h"
#include "grid_view.h"
#include "hdr_image.h"
#include "hdri_fix.h"
#include "image_io.h"
#include "info_window.h"
#include "library.h"
#include "pano.h"
#include "resample.h"
#include "settings_win.h"
#include "side_panels.h"
#include "thumb_pool.h"
#include "tonemap.h"
#include "toolbar.h"

namespace {

constexpr wchar_t kAppName[]   = L"EXRay";
constexpr wchar_t kClassName[] = L"EXRayWindow";
constexpr wchar_t kVersion[]   = L"0.10.0 beta";

// Window titles: "EXRay \u2014 v0.9.2" on its own, and
// "sky_04.exr \u2014 EXRay v0.9.2" with a file or folder open.
std::wstring AppTitle() { return std::wstring(kAppName) + L" \u2014 v" + kVersion; }
std::wstring TitleFor(const std::wstring& name) { return name + L" \u2014 " + kAppName + L" v" + kVersion; }

// Images bigger than this get a smaller copy that is re-tonemapped live
// while the exposure slider moves; the full image follows a moment later.
constexpr int kPreviewMax = 2048;
constexpr UINT kRenderDelayMs = 200;
constexpr double kMaxZoom = 32.0;
constexpr float kExposureKeyStep = 0.5f;

constexpr UINT WM_APP_LOADED   = WM_APP + 1;
constexpr UINT WM_APP_RENDERED = WM_APP + 2;
constexpr UINT WM_APP_THUMB    = WM_APP + 3;
constexpr UINT WM_APP_LAYER    = WM_APP + 4;
constexpr UINT WM_APP_EXPORTED = WM_APP + 5;
constexpr UINT WM_APP_BATCH_PROGRESS = WM_APP + 6;
constexpr UINT WM_APP_BATCH_DONE     = WM_APP + 7;
constexpr UINT WM_APP_ANALYZED  = WM_APP + 8;
constexpr UINT WM_APP_FIXED     = WM_APP + 9;
constexpr UINT WM_APP_SCANNED   = WM_APP + 10;   // a library folder was scanned
constexpr UINT WM_APP_LIBCHANGED = WM_APP + 11;  // something changed in a library folder
constexpr UINT_PTR kStatusTimer = 2;
constexpr UINT_PTR kThumbSizeTimer = 3;
constexpr UINT_PTR kPanoSharpTimer = 4;   // full-resolution 360 frame after moving stops   // re-make sharper thumbnails after a size change
constexpr UINT_PTR kRenderTimer = 1;
constexpr UINT_PTR kSearchTimer = 5;      // search a moment after typing stops
constexpr UINT_PTR kLibraryTimer = 6;     // rescan a moment after files changed
constexpr UINT_PTR kLibCheckTimer = 7;    // look for folders that were missing (unplugged drives)

enum : UINT {
    IDM_OPEN = 101, IDM_OPEN_FOLDER, IDM_REFRESH, IDM_EXIT, IDM_ABOUT, IDM_SHORTCUTS,
    IDM_GRID, IDM_IMAGE, IDM_EXPORT, IDM_COPY_PIXEL,
    IDM_LAYER_PREV, IDM_LAYER_NEXT, IDM_LAYER_MENU, IDM_SELECT_ALL, IDM_CLEAR_ISOLATE,
    IDM_THUMB_BIGGER, IDM_THUMB_SMALLER, IDM_THUMB_RESET, IDM_INVERT_DEPTH, IDM_PANO, IDM_PANO_RESET,
    IDM_FIT, IDM_ACTUAL, IDM_ZOOM_IN, IDM_ZOOM_OUT,
    IDM_PREV, IDM_NEXT, IDM_FIRST, IDM_LAST,
    IDM_EXP_UP, IDM_EXP_DOWN, IDM_EXP_RESET,
    IDM_TM_CLIP, IDM_TM_REINHARD, IDM_TM_ACES, IDM_TM_CYCLE,
    IDM_FIX, IDM_FIX_COMPARE, IDM_SAVE_FIXED, IDM_SUN_CHECK, IDM_SOURCE, IDM_SUPPORT,
    IDM_SPONSOR, IDM_ADD_FOLDER, IDM_FIND, IDM_TOGGLE_SIDEBAR, IDM_TOGGLE_INSPECTOR, IDM_UNDO_REMOVE,
    IDM_SHOW_IN_EXPLORER,
    // Folder menu in the sidebar
    IDM_FOLDER_OPEN, IDM_FOLDER_EXPLORER, IDM_FOLDER_RESCAN, IDM_FOLDER_SUB, IDM_FOLDER_REMOVE,
    IDM_LAYER_BASE = 1000,   // + layer index, from the layer pop-up menu
};

const COLORREF kBackground = RGB(0x1e, 0x1e, 0x1e);
const COLORREF kBarColor   = RGB(0x25, 0x25, 0x25);
const COLORREF kTextColor  = RGB(0xc8, 0xc8, 0xc8);
const COLORREF kDimText    = RGB(0x8a, 0x8a, 0x8a);

// --- background work results ---------------------------------------------------

struct LoadResult {
    std::wstring path;
    bool ok = false;
    std::string error;
    std::shared_ptr<const HdrImage> image;
    std::unique_ptr<HdrImage> preview;       // null when the image is small
    std::unique_ptr<Bitmap> previewBmp;
    Pyramid levels;
    TonemapSettings settings;
    double seconds = 0.0;
    std::shared_ptr<ExrDocument> doc;        // EXR with several layers, else null
    int layer = -1;
    unsigned imageId = 0;                    // for layer switches: which image this belongs to
    std::vector<uint32_t> isolate;           // Cryptomatte objects shown as a mask
    float peakLum = -1.0f, rangeStops = 0.0f; // of the decoded image (-1 = not measured)
};

struct ExportResult {
    bool ok = false;
    std::wstring path;
    std::string error;
    double seconds = 0.0;
};

struct RenderResult {
    unsigned imageId = 0;
    TonemapSettings settings;
    Pyramid levels;
};

// --- application state --------------------------------------------------------------

enum class Drag { None, Pan, Slider, Button, Scroll, SizeSlider, Panel };
enum class Mode { Single, Grid };

struct App {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    HFONT bigFont = nullptr;
    HFONT boldFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT titleFont = nullptr;
    UINT dpi = 96;
    Toolbar toolbar;

    // Loading: one file at a time; a newer request waits in `pendingPath`.
    bool loadBusy = false;
    std::wstring loadingPath;
    bool hasPending = false;
    std::wstring pendingPath;

    // The image on screen.
    unsigned imageId = 0;
    std::wstring path;
    std::shared_ptr<const HdrImage> image;
    std::unique_ptr<HdrImage> preview;
    std::unique_ptr<Bitmap> previewBmp;
    TonemapSettings previewSettings;
    Pyramid levels;
    TonemapSettings levelsSettings;
    double loadSeconds = 0.0;

    // What the user has chosen (kept when moving to another file).
    TonemapSettings settings;

    // Full-resolution re-render after a settings change (one at a time).
    bool renderBusy = false;

    // View: `zoom` = screen pixels per image pixel; (cx, cy) = image point
    // shown at the center of the viewport.
    bool fit = true;
    double zoom = 1.0;
    double cx = 0.0, cy = 0.0;

    // Mouse.
    Drag drag = Drag::None;
    POINT lastMouse{};
    ToolId hover = ToolId::None;
    ToolId pressed = ToolId::None;
    bool trackingLeave = false;

    // Other .hdr/.exr files in the same folder (refreshed when browsing).
    std::vector<std::wstring> siblings;

    // Which view is showing.
    Mode mode = Mode::Single;

    // Folder grid.
    ThumbPool* pool = nullptr;      // lives for the whole run (not freed at exit on purpose)
    GridView grid;
    std::vector<GridItem> items;
    std::wstring gridFolder;
    unsigned gridGen = 0;
    int selected = -1;
    int gridHover = -1;
    bool scrollHot = false;
    int grabOffset = 0;              // where the scrollbar thumb was grabbed

    // EXR layers of the image on screen (null for single-layer files).
    std::shared_ptr<ExrDocument> doc;
    int layer = -1;
    bool layerBusy = false;          // a layer is being prepared
    int layerTarget = -1;            // ...which one
    bool hasPendingView = false;     // a layer / isolation asked for while busy
    int pendingLayer = -1;
    std::vector<uint32_t> pendingIsolate;
    std::vector<uint32_t> isolate;   // Cryptomatte objects isolated on screen
    RECT layerChip{};                // clickable layer name in the info bar
    bool chipHot = false;

    // Pixel under the mouse.
    bool pixelValid = false;
    int pixelX = 0, pixelY = 0;
    float pixel[3] = {0, 0, 0};
    std::vector<CryptoHit> pixelHits;   // Cryptomatte layers: objects under the mouse

    // Export.
    bool exportBusy = false;
    int exportLookUsed = 0;          // for the "Exported ..." message (-1 = a data view)
    std::wstring status;             // short message in the info bar
    bool batchBusy = false;
    int batchDone = 0, batchTotal = 0;
    std::wstring batchFolder;
    std::shared_ptr<std::atomic<bool>> batchCancel;

    // Grid selection anchor (Shift+click / Shift+arrows) and thumbnail size.
    int anchor = -1;
    float thumbScale = 1.0f;
    int jobBoxW = 0, jobBoxH = 0;    // thumbnail box size the workers were last given
    RECT sizeSlider{};               // in the info bar, grid only
    bool sizeHot = false;

    // A click (not a drag) on the image: used to pick Cryptomatte objects.
    POINT downPoint{};
    bool movedSinceDown = false;

    AppSettings cfg;                 // loaded from / saved to the .ini
    ViewOptions viewOpts;            // e.g. inverted depth view

    // 360-degree view.
    bool panoOn = false;             // chosen by the user; used whenever the image is 2:1
    PanoView pano;
    bool panoFast = false;           // half resolution while moving
    std::unique_ptr<Bitmap> panoBmp; // the rendered view
    // What panoBmp currently shows, so repaints that don't change the view
    // (pixel readout, toolbar hover...) reuse it instead of rendering again.
    struct PanoKey {
        const void* source = nullptr; unsigned imageId = 0; TonemapSettings look;
        double yaw = 1e9, pitch = 0, fov = 0; int w = 0, h = 0;
        bool operator==(const PanoKey& o) const {
            return source == o.source && imageId == o.imageId && look == o.look && yaw == o.yaw &&
                   pitch == o.pitch && fov == o.fov && w == o.w && h == o.h;
        }
    } panoKey;

    // Fix HDRI. While a fix is applied, `fixOriginal` keeps the image as
    // loaded and `fixedImage` the fixed copy; g.image is whichever is shown.
    bool fixBusy = false;            // checking or fixing in the background
    std::wstring fixBusyText;
    std::shared_ptr<const HdrImage> fixOriginal;
    std::shared_ptr<const HdrImage> fixedImage;
    bool showingOriginal = false;    // B: before / after
    bool fixSaved = false;
    bool fixDiscarded = false;       // the user agreed to drop it; cleared when the next image arrives
    HdriReport fixReport;            // analysis of the original
    std::weak_ptr<const HdrImage> fixReportFor;   // the image fixReport describes
    HdriFixOptions fixOpts;
    bool fixOptsValid = false;       // fixOpts were chosen for fixReport (kept after "Sun check")

    // Sun check: the 360 view aimed at the sun, exposed for a real sun; S again restores this.
    struct SunCheckSaved {
        bool on = false;
        TonemapSettings look;
        bool panoOn = false;
        PanoView pano;
        bool fit = true;
        double zoom = 1.0, cx = 0.0, cy = 0.0;
    } sunCheck;
    HdriFixResult fixResult;
    HWND fixPanel = nullptr;         // Fix HDRI, docked over the inspector
    bool fixPanelOn = false;

    // Studio layout: folder library on the left, inspector on the right.
    FolderLibrary library;
    std::map<std::wstring, std::unique_ptr<ScanResult>> scans;   // by library folder (lower case); missing while scanning
    std::vector<std::wstring> searchHitPaths;
    std::set<std::wstring> expanded;                  // folders showing their subfolders (lower case)
    std::vector<TreeRow> rows;                        // the tree as drawn
    Sidebar sidebar;
    SidebarHit sideHover;
    Inspector inspector;
    int inspHover = 0, inspPressed = 0;
    bool gridSubfolders = false;     // the grid shows the files of subfolders too
    // A thumbnail of the selected file for the inspector (grid tiles free theirs when off screen).
    std::unique_ptr<Bitmap> inspBmp;
    std::wstring inspBmpPath;
    TonemapSettings inspBmpSettings;
    // Brightest value and range of the image on screen (measured once per image).
    const HdrImage* statsFor = nullptr;
    float imagePeak = 0.0f, imageRange = 0.0f;

    // Search (the box at the top of the sidebar).
    HWND searchEdit = nullptr;
    WNDPROC searchEditProc = nullptr;
    HBRUSH editBrush = nullptr;
    bool searchActive = false;       // the grid shows search results
    bool searchAll = true;           // all library folders, else the folder that was showing
    std::wstring searchQuery;
    std::wstring searchFolder;       // the folder showing when the search started
    std::vector<int> rowMatches;     // matches per tree row
    size_t searchHits = 0;
} g;

// --- small helpers -------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

int Scale(int px) { return MulDiv(px, static_cast<int>(g.dpi), 96); }
bool HasImage() { return g.image && !g.image->empty(); }

// The download page; paying there is optional (EXRay is free).
constexpr wchar_t kDownloadPage[] = L"https://p1xelrise.gumroad.com/l/exray";
// The source code (GNU GPL v3 or later).
constexpr wchar_t kSourcePage[] = L"https://github.com/p1xelrise/EXRay";
constexpr wchar_t kSponsorPage[] = L"https://github.com/sponsors/p1xelrise";
bool CtrlDown() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }

HFONT UiFont(int px, int weight) {
    return CreateFontW(-Scale(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void RecreateFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.bigFont) DeleteObject(g.bigFont);
    for (HFONT* f : {&g.boldFont, &g.smallFont, &g.titleFont}) if (*f) DeleteObject(*f);
    g.boldFont = UiFont(13, FW_SEMIBOLD);
    g.smallFont = UiFont(12, FW_NORMAL);
    g.titleFont = UiFont(15, FW_SEMIBOLD);
    g.font = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g.bigFont = CreateFontW(-Scale(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

int InfoBarHeight() { return Scale(28); }

RECT ClientRect() { RECT r; GetClientRect(g.hwnd, &r); return r; }

// Between the toolbar and the info bar, across the whole width.
RECT MiddleRect() {
    RECT r = ClientRect();
    r.top = std::min<LONG>(r.bottom, g.toolbar.Height());
    r.bottom = std::max(r.top, r.bottom - InfoBarHeight());
    return r;
}

// The side panels give way on narrow windows so the image keeps room.
int MinContentWidth() { return Scale(440); }
bool SidebarShown() {
    const RECT m = MiddleRect();
    return g.cfg.showSidebar && (m.right - m.left) - Sidebar::Width(g.dpi) >= MinContentWidth();
}
bool InspectorShown() {
    const RECT m = MiddleRect();
    const int side = SidebarShown() ? Sidebar::Width(g.dpi) : 0;
    const bool room = (m.right - m.left) - side - Inspector::Width(g.dpi) >= MinContentWidth();
    return (g.cfg.showInspector || g.fixPanelOn) && (room || g.fixPanelOn);
}
RECT SidebarRect() {
    RECT r = MiddleRect();
    r.right = SidebarShown() ? r.left + Sidebar::Width(g.dpi) : r.left;
    return r;
}
RECT InspectorRect() {
    RECT r = MiddleRect();
    r.left = InspectorShown() ? std::max<LONG>(SidebarRect().right, r.right - Inspector::Width(g.dpi)) : r.right;
    return r;
}
// Grid or image area.
RECT ContentRect() {
    RECT r = MiddleRect();
    r.left = SidebarRect().right;
    r.right = std::max(r.left, InspectorRect().left);
    return r;
}
int GridHeaderHeight() { return Scale(40); }
RECT GridHeaderRect() {
    RECT r = ContentRect();
    r.bottom = std::min<LONG>(r.bottom, r.top + GridHeaderHeight());
    return r;
}

RECT Viewport() {
    RECT r = ContentRect();
    if (g.mode == Mode::Grid) r.top = std::min(r.bottom, GridHeaderRect().bottom);
    return r;
}

bool InViewport(POINT p) { RECT vp = Viewport(); return PtInRect(&vp, p) != FALSE; }

void InvalidateToolbar() {
    RECT r = ClientRect();
    r.bottom = g.toolbar.Height();
    InvalidateRect(g.hwnd, &r, FALSE);
}

void Redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }

// The file we are showing or about to show (used for browsing).
const std::wstring& TargetPath() {
    if (g.hasPending) return g.pendingPath;
    if (g.loadBusy) return g.loadingPath;
    return g.path;
}

bool PanoActive();
void SetStatus(const std::wstring& text);

// --- view (zoom & pan) ----------------------------------------------------------------

double FitZoom() {
    if (!HasImage()) return 1.0;
    RECT vp = Viewport();
    double w = std::max(1L, vp.right - vp.left), h = std::max(1L, vp.bottom - vp.top);
    return std::min({w / g.image->width, h / g.image->height, 1.0});   // never enlarge in Fit
}

void ClampView() {
    if (!HasImage()) return;
    RECT vp = Viewport();
    const double visW = (vp.right - vp.left) / g.zoom, visH = (vp.bottom - vp.top) / g.zoom;
    const double W = g.image->width, H = g.image->height;
    g.cx = (visW >= W) ? W / 2 : std::clamp(g.cx, visW / 2, W - visW / 2);
    g.cy = (visH >= H) ? H / 2 : std::clamp(g.cy, visH / 2, H - visH / 2);
}

void SetFit() {
    g.fit = true;
    if (!HasImage()) return;
    g.zoom = FitZoom();
    g.cx = g.image->width / 2.0;
    g.cy = g.image->height / 2.0;
}

bool CanPan() {
    if (!HasImage()) return false;
    if (PanoActive()) return true;
    RECT vp = Viewport();
    return g.image->width * g.zoom > (vp.right - vp.left) + 0.5 ||
           g.image->height * g.zoom > (vp.bottom - vp.top) + 0.5;
}

// Zooms so that the image point under `p` (client coords) stays put.
void ZoomAround(POINT p, double newZoom) {
    if (!HasImage()) return;
    const double fitZ = FitZoom();
    // Snap when passing 100% or the fitted size, so both are easy to hit.
    if ((g.zoom < 1.0 && newZoom > 1.0) || (g.zoom > 1.0 && newZoom < 1.0)) newZoom = 1.0;
    if (g.zoom > fitZ && newZoom <= fitZ) { SetFit(); Redraw(); return; }
    newZoom = std::clamp(newZoom, fitZ, kMaxZoom);

    RECT vp = Viewport();
    const double vcx = (vp.left + vp.right) / 2.0, vcy = (vp.top + vp.bottom) / 2.0;
    const double ix = g.cx + (p.x - vcx) / g.zoom, iy = g.cy + (p.y - vcy) / g.zoom;
    g.zoom = newZoom;
    g.cx = ix - (p.x - vcx) / g.zoom;
    g.cy = iy - (p.y - vcy) / g.zoom;
    g.fit = std::fabs(newZoom - fitZ) < 1e-9;
    ClampView();
    Redraw();
}

POINT ViewportCenter() {
    RECT vp = Viewport();
    return {(vp.left + vp.right) / 2, (vp.top + vp.bottom) / 2};
}

// The 360 view is used when it's switched on and the image is shaped 2:1.
bool PanoActive() {
    return g.panoOn && g.mode == Mode::Single && HasImage() && LooksPanoramic(g.image->width, g.image->height);
}

// Screen point -> image pixel, for the flat view and the 360 view alike.
bool ScreenToImage(POINT p, int& x, int& y) {
    if (!HasImage()) return false;
    RECT vp = Viewport();
    if (PanoActive()) {
        PanoScreenToImage(g.pano, vp.right - vp.left, vp.bottom - vp.top, p.x - vp.left + 0.5, p.y - vp.top + 0.5,
                          g.image->width, g.image->height, x, y);
        return true;
    }
    const double vcx = (vp.left + vp.right) / 2.0, vcy = (vp.top + vp.bottom) / 2.0;
    x = static_cast<int>(std::floor(g.cx + (p.x + 0.5 - vcx) / g.zoom));
    y = static_cast<int>(std::floor(g.cy + (p.y + 0.5 - vcy) / g.zoom));
    return x >= 0 && y >= 0 && x < g.image->width && y < g.image->height;
}

// Moving the 360 view: draw at half resolution until it rests for a moment.
void PanoMoved() {
    NormalizePanoView(g.pano);
    g.panoFast = true;
    SetTimer(g.hwnd, kPanoSharpTimer, 150, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void ResetPanoView() {
    g.pano = PanoView{};
    g.panoFast = false;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void ZoomActualSize(POINT at) {
    if (!HasImage()) return;
    if (FitZoom() >= 1.0) { SetFit(); Redraw(); return; }   // already shown at 100%
    ZoomAround(at, 1.0);
}

// --- tonemap settings -------------------------------------------------------------------

void RequestFullRender();
void ClearFix();
void CopyPixel();
void UpdatePoolPause();
void Command(UINT id);

// Brings the on-screen image in line with g.settings: small images are
// redone right away; big ones update the quick preview now and the full
// image shortly after.
void ApplySettings() {
    if (g.mode == Mode::Grid) { Redraw(); return; }   // thumbnails update as they're drawn
    if (!HasImage()) { InvalidateToolbar(); return; }
    if (!g.preview) {
        if (g.levels.empty() || g.levelsSettings != g.settings) {
            try {
                g.levels = BuildPyramid(*g.image, g.settings);
                g.levelsSettings = g.settings;
            } catch (const std::bad_alloc&) { /* keep the old levels */ }
        }
    } else {
        if (g.previewBmp && g.previewSettings != g.settings) {
            TonemapToBGRA8(*g.preview, g.settings, g.previewBmp->bits());
            GdiFlush();
            g.previewSettings = g.settings;
        }
        if (g.levelsSettings != g.settings) SetTimer(g.hwnd, kRenderTimer, kRenderDelayMs, nullptr);
    }
    Redraw();
}

void SetSettings(TonemapSettings s) {
    // The Sun check may go far below the slider's range (a real sun is 100,000+).
    s.exposureEV = std::clamp(std::round(s.exposureEV * 10.0f) / 10.0f, g.sunCheck.on ? kSunCheckMinEV : kMinExposureEV,
                              kMaxExposureEV);
    if (s == g.settings) return;
    g.settings = s;
    ApplySettings();
}

void SetExposure(float ev) { TonemapSettings s = g.settings; s.exposureEV = ev; SetSettings(s); }
void SetCurve(Tonemap c) { TonemapSettings s = g.settings; s.curve = c; SetSettings(s); }

void RequestFullRender() {
    KillTimer(g.hwnd, kRenderTimer);
    if (!HasImage() || !g.preview || g.renderBusy) return;
    if (!g.levels.empty() && g.levelsSettings == g.settings) return;

    g.renderBusy = true;
    HWND hwnd = g.hwnd;
    std::shared_ptr<const HdrImage> image = g.image;
    unsigned id = g.imageId;
    TonemapSettings settings = g.settings;
    std::thread([hwnd, image, id, settings]() {
        auto res = std::make_unique<RenderResult>();
        res->imageId = id;
        res->settings = settings;
        try { res->levels = BuildPyramid(*image, settings); } catch (...) { res->levels.clear(); }
        if (PostMessageW(hwnd, WM_APP_RENDERED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();   // info bar shows "updating"
}

void OnRendered(RenderResult* raw) {
    std::unique_ptr<RenderResult> res(raw);
    g.renderBusy = false;
    if (res->imageId == g.imageId && res->settings == g.settings && !res->levels.empty()) {
        g.levels = std::move(res->levels);
        g.levelsSettings = res->settings;
        g.panoKey = App::PanoKey{};
    }
    res.reset();               // free a stale result before possibly starting another
    RequestFullRender();       // settings may have moved on while we worked
    Redraw();
}

// --- loading ----------------------------------------------------------------------------

// Everything needed to show `img`: a quick preview for big images, and the
// tonemapped display levels. Runs on a background thread.
void PrepareDisplay(LoadResult& res, std::shared_ptr<HdrImage> img, const TonemapSettings& settings) {
    HdrImage preview = MakePreviewHdr(*img, kPreviewMax, kPreviewMax);
    if (!preview.empty()) {
        res.preview = std::make_unique<HdrImage>(std::move(preview));
        res.previewBmp = TonemapToBitmap(*res.preview, settings);
    }
    res.levels = BuildPyramid(*img, settings);
    res.image = std::move(img);
}

void LaunchLoad(const std::wstring& path) {
    g.loadBusy = true;
    g.loadingPath = path;
    UpdatePoolPause();   // give the full image the memory and CPU

    HWND hwnd = g.hwnd;
    TonemapSettings settings = g.settings;
    std::thread([hwnd, path, settings]() {
        auto res = std::make_unique<LoadResult>();
        res->path = path;
        res->settings = settings;
        auto t0 = std::chrono::steady_clock::now();
        try {
            auto img = std::make_shared<HdrImage>();
            {
                std::vector<unsigned char> data;
                res->ok = ReadFileBytes(path, data, res->error) &&
                          DecodeHdrImage(data, *img, res->error, &res->doc);
            }   // file bytes freed here
            if (res->ok) {
                res->layer = res->doc ? res->doc->DefaultLayer() : -1;
                ImageQuickStats(*img, res->peakLum, res->rangeStops);
                PrepareDisplay(*res, std::move(img), settings);
            }
        } catch (const std::bad_alloc&) {
            res->ok = false;
            res->error = "Not enough memory to open an image this large.";
        } catch (...) {
            res->ok = false;
            res->error = "Something went wrong while opening the file.";
        }
        if (!res->ok) { res->image.reset(); res->preview.reset(); res->previewBmp.reset(); res->levels.clear(); res->doc.reset(); }
        res->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (PostMessageW(hwnd, WM_APP_LOADED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

bool ConfirmDiscardFix();
void CloseFixPanel();

void StartLoad(const std::wstring& path) {
    if (!ConfirmDiscardFix()) return;
    if (g.loadBusy) {               // wait for the current load; only the newest request counts
        g.hasPending = true;
        g.pendingPath = path;
        Redraw();
        return;
    }
    LaunchLoad(path);
}

void RefreshSiblings(const std::wstring& path) {
    g.siblings = ListHdrFilesInFolder(FolderOf(path));
}

void InstallImage(LoadResult& res) {
    g.panoKey = App::PanoKey{};   // new pixels: the cached 360 frame is out of date
    // Drop the old image first so two big images aren't held longer than needed.
    g.levels.clear();
    g.previewBmp.reset();
    g.preview.reset();
    g.image.reset();

    g.image = std::move(res.image);
    g.preview = std::move(res.preview);
    g.previewBmp = std::move(res.previewBmp);
    g.previewSettings = res.settings;
    g.levels = std::move(res.levels);
    g.levelsSettings = res.settings;
}

void OnLoaded(LoadResult* raw) {
    std::unique_ptr<LoadResult> res(raw);
    g.loadBusy = false;
    UpdatePoolPause();

    if (g.hasPending) {             // the user already moved on; skip this one
        g.hasPending = false;
        res.reset();
        LaunchLoad(g.pendingPath);
        return;
    }

    if (!res->ok) {
        Redraw();
        std::wstring msg = FileNameOf(res->path) + L"\n\n" + Utf8ToWide(res->error);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }

    ++g.imageId;
    g.path = res->path;
    ClearFix();
    CloseFixPanel();   // the report was for the previous image
    InstallImage(*res);
    g.statsFor = g.image.get();
    g.imagePeak = res->peakLum;
    g.imageRange = res->rangeStops;
    g.loadSeconds = res->seconds;
    g.doc = std::move(res->doc);
    g.layer = res->layer;
    g.hasPendingView = false;
    g.isolate.clear();
    g.pixelValid = false;
    RefreshSiblings(g.path);

    if (g.mode == Mode::Single) {
        SetWindowTextW(g.hwnd, TitleFor(FileNameOf(g.path)).c_str());
    }

    SetFit();
    g.panoBmp.reset();
    g.panoKey = App::PanoKey{};
    if (g.sunCheck.on) {   // the sun check was for the previous image: keep the view, restore the look
        g.sunCheck.on = false;
        g.panoOn = g.sunCheck.panoOn;
        res->settings = g.settings;   // compared below
        SetSettings(g.sunCheck.look);
    }
    if (g.panoOn && !LooksPanoramic(g.image->width, g.image->height))
        SetStatus(L"Not a 2:1 panorama \u2014 shown flat");
    if (g.settings != res->settings) ApplySettings();   // slider moved during loading
    Redraw();
}

void ShowOpenDialog() {
    static wchar_t file[32768];
    file[0] = L'\0';
    std::wstring initialDir = FolderOf(TargetPath());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"HDR images (*.exr; *.hdr)\0*.exr;*.hdr\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.lpstrTitle = L"Open HDR or EXR image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        if (!ConfirmDiscardFix()) return;
        g.mode = Mode::Single;
        StartLoad(file);
    }
}

// --- EXR layers and Cryptomatte ------------------------------------------------------------

int LayerCount() { return g.doc ? static_cast<int>(g.doc->Layers().size()) : 0; }

// The layer that is showing or about to show.
int CurrentLayer() {
    if (g.hasPendingView) return g.pendingLayer;
    if (g.layerBusy) return g.layerTarget;
    return g.layer;
}

// Cryptomatte set of the layer on screen, or -1.
int CurrentCrypto() {
    if (!g.doc || g.layer < 0 || g.layer >= LayerCount() || g.mode != Mode::Single) return -1;
    return g.doc->Layers()[static_cast<size_t>(g.layer)].crypto;
}

// Builds layer `index` (for a Cryptomatte: the ID view, or a mask of the
// objects in `isolate`) on a background thread.
void LaunchLayer(int index, std::vector<uint32_t> isolate) {
    g.layerBusy = true;
    g.layerTarget = index;
    HWND hwnd = g.hwnd;
    std::shared_ptr<ExrDocument> doc = g.doc;
    TonemapSettings settings = g.settings;
    unsigned id = g.imageId;
    std::wstring path = g.path;
    ViewOptions opts = g.viewOpts;
    std::thread([hwnd, doc, settings, id, path, index, isolate, opts]() {
        auto res = std::make_unique<LoadResult>();
        res->path = path;
        res->settings = settings;
        res->imageId = id;
        res->layer = index;
        res->isolate = isolate;
        try {
            auto img = std::make_shared<HdrImage>();
            const ExrLayer& L = doc->Layers()[static_cast<size_t>(index)];
            res->ok = (L.crypto >= 0 && !isolate.empty())
                          ? doc->BuildCryptoView(L.crypto, isolate, *img, res->error)
                          : doc->BuildLayer(index, *img, res->error, opts);
            if (res->ok) PrepareDisplay(*res, std::move(img), settings);
        } catch (const std::bad_alloc&) {
            res->ok = false;
            res->error = "Not enough memory to show this layer.";
        }
        if (!res->ok) { res->image.reset(); res->preview.reset(); res->previewBmp.reset(); res->levels.clear(); }
        if (PostMessageW(hwnd, WM_APP_LAYER, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
}

// Shows layer `index` with the given Cryptomatte isolation (empty = none).
void ShowLayerView(int index, std::vector<uint32_t> isolate) {
    if (!g.doc || g.mode != Mode::Single || g.loadBusy) return;
    if (index < 0 || index >= LayerCount()) { MessageBeep(MB_OK); return; }
    if (index != g.layer && !ConfirmDiscardFix()) return;
    if (g.layerBusy) {
        g.hasPendingView = true;
        g.pendingLayer = index;
        g.pendingIsolate = std::move(isolate);
        Redraw();
        return;
    }
    if (index == g.layer && isolate == g.isolate) return;
    LaunchLayer(index, std::move(isolate));
    Redraw();
}

void SwitchLayer(int index) { ShowLayerView(index, {}); }

// The view of the layer on screen (colour, depth, normal, vector).
LayerView CurrentView() {
    if (!g.doc || g.layer < 0 || g.layer >= LayerCount()) return LayerView::Color;
    return g.doc->Layers()[static_cast<size_t>(g.layer)].view;
}

void SetStatus(const std::wstring& text);

void ToggleInvertDepth() {
    g.viewOpts.invertDepth = !g.viewOpts.invertDepth;
    if (CurrentView() == LayerView::Depth && !g.layerBusy) LaunchLayer(g.layer, g.isolate);
    SetStatus(g.viewOpts.invertDepth ? L"Depth view inverted: near is black" : L"Depth view: near is white");
}

void SetIsolation(std::vector<uint32_t> ids) {
    if (CurrentCrypto() < 0) return;
    ShowLayerView(g.layer, std::move(ids));
}

// A click on a Cryptomatte view: isolate the object under the mouse. Ctrl+click
// adds or removes objects; a click on empty space shows all objects again.
void CryptoClick(POINT p, bool add) {
    const int set = CurrentCrypto();
    if (set < 0 || !HasImage()) return;
    int x = -1, y = -1;
    ScreenToImage(p, x, y);
    std::vector<CryptoHit> hits = g.doc->CryptoAt(set, x, y);
    std::vector<uint32_t> ids = g.isolate;
    if (hits.empty()) {
        if (!add) ids.clear();
    } else {
        const uint32_t id = hits[0].id;
        auto it = std::find(ids.begin(), ids.end(), id);
        if (add) {
            if (it != ids.end()) ids.erase(it); else ids.push_back(id);
        } else {
            ids = (ids.size() == 1 && ids[0] == id) ? std::vector<uint32_t>{} : std::vector<uint32_t>{id};
        }
    }
    SetIsolation(std::move(ids));
}

void OnLayerLoaded(LoadResult* raw) {
    std::unique_ptr<LoadResult> res(raw);
    g.layerBusy = false;
    if (res->imageId != g.imageId) {
        // Another file is showing now. A view asked for since then belongs to it.
        res.reset();
        if (g.hasPendingView) {
            g.hasPendingView = false;
            if (g.doc && !g.loadBusy) LaunchLayer(g.pendingLayer, std::move(g.pendingIsolate));
        }
        Redraw();
        return;
    }

    if (res->ok) {
        const bool sameSize = HasImage() && res->image->width == g.image->width &&
                              res->image->height == g.image->height;
        if (res->layer != g.layer) ClearFix();
        InstallImage(*res);
        g.layer = res->layer;
        g.isolate = std::move(res->isolate);
        g.pixelValid = false;
        if (!sameSize || g.fit) SetFit(); else ClampView();   // keep zoom & position when possible
        if (g.settings != res->settings) ApplySettings();
    } else {
        std::wstring msg = Utf8ToWide(res->error);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
    res.reset();

    if (g.hasPendingView) {
        g.hasPendingView = false;
        if (g.pendingLayer != g.layer || g.pendingIsolate != g.isolate)
            LaunchLayer(g.pendingLayer, std::move(g.pendingIsolate));
    }
    Redraw();
}

void ShowLayerMenu() {
    if (!g.doc || g.mode != Mode::Single) return;
    HMENU m = CreatePopupMenu();
    const auto& layers = g.doc->Layers();
    const int current = CurrentLayer();
    for (size_t i = 0; i < layers.size(); ++i) {
        std::wstring label = Utf8ToWide(ExrDocument::DisplayName(layers[i])) + L"\t" + Utf8ToWide(layers[i].channels);
        for (size_t k = 0; k < label.size(); ++k)   // '&' would become an underline
            if (label[k] == L'&') label.insert(k++, 1, L'&');
        AppendMenuW(m, MF_STRING | (static_cast<int>(i) == current ? MF_CHECKED : 0),
                    IDM_LAYER_BASE + static_cast<UINT>(i), label.c_str());
    }
    POINT pt{g.layerChip.left, g.layerChip.top};
    if (IsRectEmpty(&g.layerChip)) { RECT vp = Viewport(); pt = {vp.left + Scale(10), vp.bottom}; }
    ClientToScreen(g.hwnd, &pt);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= IDM_LAYER_BASE) SwitchLayer(static_cast<int>(cmd - IDM_LAYER_BASE));
}

std::wstring IsolationText() {
    const int set = CurrentCrypto();
    if (set < 0 || g.isolate.empty()) return {};
    std::wstring t = L"Isolated: " + Utf8ToWide(g.doc->CryptoName(set, g.isolate[0]));
    if (g.isolate.size() > 1) t += L" + " + std::to_wstring(g.isolate.size() - 1) + L" more";
    return t;
}

// --- export ------------------------------------------------------------------------------------

// The look used for exported colour images: 0 = Clip (raw sRGB, exposure 0),
// 1 = ACES (exposure 0), 2 = exactly what's on screen.
TonemapSettings LookSettings(int look) {
    if (look == 1) return TonemapSettings{0.0f, Tonemap::ACES};
    if (look == 2) return g.settings;
    return TonemapSettings{0.0f, Tonemap::Clip};
}

const wchar_t* LookName(int look) {
    return look == 1 ? L"ACES" : look == 2 ? L"as shown" : L"Clip";
}

bool ViewIsData() {   // the image on screen is a Cryptomatte or data-pass view, not colour
    return HasImage() && g.image->displayReferred;
}

void SetStatus(const std::wstring& text) {
    g.status = text;
    KillTimer(g.hwnd, kStatusTimer);
    if (!text.empty()) SetTimer(g.hwnd, kStatusTimer, 6000, nullptr);
    Redraw();
}

std::wstring SafeFilePart(std::wstring s) {
    for (auto& c : s)
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    return s;
}

void UpdatePoolPause() {
    if (g.pool) g.pool->SetPaused(g.loadBusy || g.batchBusy);
}

// Image view: save what's on screen (the current layer, or the Cryptomatte /
// data-pass view). Colour images use the chosen look (Clip by default).
void ShowExportDialog() {
    if (g.mode != Mode::Single || !HasImage()) return;
    if (g.exportBusy || g.batchBusy) { MessageBeep(MB_OK); return; }

    std::wstring stem = FileNameOf(g.path);
    if (size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    if (g.doc && g.layer >= 0 && g.layer != g.doc->DefaultLayer())
        stem += L"_" + SafeFilePart(Utf8ToWide(ExrDocument::DisplayName(g.doc->Layers()[static_cast<size_t>(g.layer)])));
    if (const int set = CurrentCrypto(); set >= 0 && !g.isolate.empty())
        stem += L"_" + SafeFilePart(Utf8ToWide(g.doc->CryptoName(set, g.isolate[0])));

    enum : DWORD { kLookGroup = 201, kLookBox = 202 };
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"PNG image", L"*.png"}, {L"JPEG image", L"*.jpg;*.jpeg"}};
    dlg->SetFileTypes(2, types);
    dlg->SetFileTypeIndex(g.cfg.exportJpeg ? 2 : 1);
    dlg->SetDefaultExtension(g.cfg.exportJpeg ? L"jpg" : L"png");
    dlg->SetFileName(stem.c_str());
    dlg->SetTitle(L"Export image");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN);
    const std::wstring initialDir = !g.cfg.exportFolder.empty() &&
                                    GetFileAttributesW(g.cfg.exportFolder.c_str()) != INVALID_FILE_ATTRIBUTES
                                        ? g.cfg.exportFolder : FolderOf(g.path);
    if (!initialDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    const bool data = ViewIsData();
    IFileDialogCustomize* cust = nullptr;
    if (!data && SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&cust)))) {
        cust->StartVisualGroup(kLookGroup, L"Look:");
        cust->AddComboBox(kLookBox);
        cust->AddControlItem(kLookBox, 0, L"Clip \u2014 raw sRGB, exposure 0");
        cust->AddControlItem(kLookBox, 1, L"ACES \u2014 exposure 0");
        cust->AddControlItem(kLookBox, 2, L"As shown on screen");
        cust->SetSelectedControlItem(kLookBox, static_cast<DWORD>(std::clamp(g.cfg.exportLook, 0, 2)));
        cust->EndVisualGroup();
    }

    std::wstring path;
    UINT typeIndex = 1;
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
        dlg->GetFileTypeIndex(&typeIndex);
        if (cust) {
            DWORD sel = 0;
            if (SUCCEEDED(cust->GetSelectedControlItem(kLookBox, &sel))) g.cfg.exportLook = static_cast<int>(sel);
        }
    }
    if (cust) cust->Release();
    dlg->Release();
    if (path.empty()) return;

    // If the name has no image extension, follow the chosen file type.
    std::wstring lower = path;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    auto ends = [&](const wchar_t* e) { size_t n = wcslen(e); return lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0; };
    if (!ends(L".png") && !ends(L".jpg") && !ends(L".jpeg")) path += (typeIndex == 2) ? L".jpg" : L".png";

    g.cfg.exportFolder = FolderOf(path);
    g.cfg.exportJpeg = ExportFormatFromPath(path) == ExportFormat::Jpeg;
    SaveSettings(g.cfg);

    g.exportBusy = true;
    g.exportLookUsed = data ? -1 : g.cfg.exportLook;
    SetStatus(L"");
    HWND hwnd = g.hwnd;
    std::shared_ptr<const HdrImage> image = g.image;
    const TonemapSettings look = LookSettings(g.cfg.exportLook);
    std::thread([hwnd, image, path, look]() {
        auto res = std::make_unique<ExportResult>();
        res->path = path;
        auto t0 = std::chrono::steady_clock::now();
        res->ok = ExportImage(*image, look, path, ExportFormatFromPath(path), res->error);
        res->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (PostMessageW(hwnd, WM_APP_EXPORTED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

void OnExported(ExportResult* raw) {
    std::unique_ptr<ExportResult> res(raw);
    g.exportBusy = false;
    if (res->ok && g.exportLookUsed == -2) {
        g.fixSaved = true;
        SetStatus(L"Saved " + FileNameOf(res->path));
    } else if (res->ok) {
        SetStatus(L"Exported " + FileNameOf(res->path) +
                  (g.exportLookUsed < 0 ? std::wstring() : L" (" + std::wstring(LookName(g.exportLookUsed)) + L")"));
    } else {
        Redraw();
        std::wstring msg = (g.exportLookUsed == -2 ? L"Couldn't save " : L"Couldn't export ") + FileNameOf(res->path) +
                           L"\n\n" + Utf8ToWide(res->error);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
}

// --- batch export from the grid -------------------------------------------------------------

std::vector<std::wstring> PickedPaths() {
    std::vector<std::wstring> paths;
    for (const auto& it : g.items) if (it.picked) paths.push_back(it.path);
    if (paths.empty() && g.selected >= 0 && g.selected < static_cast<int>(g.items.size()))
        paths.push_back(g.items[static_cast<size_t>(g.selected)].path);
    return paths;
}

int PickedCount() {
    int n = 0;
    for (const auto& it : g.items) n += it.picked ? 1 : 0;
    return n;
}

// Folder picker with two extra controls: the file type and the "every layer" choice.
bool AskBatchExport(int count, std::wstring& folder, bool& jpeg, bool& everyLayer, int& look) {
    enum : DWORD { kFormatBox = 101, kEveryLayer = 102, kNote = 103, kLookGroup = 104, kLookBox = 105 };
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    std::wstring title = L"Export " + std::to_wstring(count) + (count == 1 ? L" image" : L" images") + L" to a folder";
    dlg->SetTitle(title.c_str());
    dlg->SetOkButtonLabel(L"Export here");

    std::wstring start = !g.cfg.exportFolder.empty() &&
                         GetFileAttributesW(g.cfg.exportFolder.c_str()) != INVALID_FILE_ATTRIBUTES
                             ? g.cfg.exportFolder : g.gridFolder;
    if (!start.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }

    IFileDialogCustomize* cust = nullptr;
    if (SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&cust)))) {
        cust->StartVisualGroup(kNote, L"Save as:");
        cust->AddComboBox(kFormatBox);
        cust->AddControlItem(kFormatBox, 0, L"PNG");
        cust->AddControlItem(kFormatBox, 1, L"JPEG");
        cust->SetSelectedControlItem(kFormatBox, jpeg ? 1 : 0);
        cust->EndVisualGroup();
        cust->StartVisualGroup(kLookGroup, L"Look:");
        cust->AddComboBox(kLookBox);
        cust->AddControlItem(kLookBox, 0, L"Clip \u2014 raw sRGB, exposure 0");
        cust->AddControlItem(kLookBox, 1, L"ACES \u2014 exposure 0");
        cust->AddControlItem(kLookBox, 2, L"As shown on screen");
        cust->SetSelectedControlItem(kLookBox, static_cast<DWORD>(std::clamp(look, 0, 2)));
        cust->EndVisualGroup();
        cust->AddCheckButton(kEveryLayer, L"Export every layer of multi-layer EXRs", everyLayer ? TRUE : FALSE);
    }

    bool ok = false;
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
        if (ok && cust) {
            DWORD sel = 0;
            if (SUCCEEDED(cust->GetSelectedControlItem(kFormatBox, &sel))) jpeg = sel == 1;
            BOOL checked = FALSE;
            if (SUCCEEDED(cust->GetCheckButtonState(kEveryLayer, &checked))) everyLayer = checked != FALSE;
            if (SUCCEEDED(cust->GetSelectedControlItem(kLookBox, &sel))) look = static_cast<int>(sel);
        }
    }
    if (cust) cust->Release();
    dlg->Release();
    return ok;
}

void StartBatchExport() {
    if (g.batchBusy || g.exportBusy) { MessageBeep(MB_OK); return; }
    std::vector<std::wstring> files = PickedPaths();
    if (files.empty()) return;

    std::wstring folder;
    bool jpeg = g.cfg.exportJpeg, every = g.cfg.exportEveryLayer;
    int look = g.cfg.exportLook;
    if (!AskBatchExport(static_cast<int>(files.size()), folder, jpeg, every, look)) return;
    g.cfg.exportFolder = folder;
    g.cfg.exportJpeg = jpeg;
    g.cfg.exportEveryLayer = every;
    g.cfg.exportLook = look;
    SaveSettings(g.cfg);

    BatchJob job;
    job.files = std::move(files);
    job.outFolder = folder;
    job.format = jpeg ? ExportFormat::Jpeg : ExportFormat::Png;
    job.everyLayer = every;
    job.look = LookSettings(look);

    g.batchBusy = true;
    g.batchDone = 0;
    g.batchTotal = static_cast<int>(job.files.size());
    g.batchFolder = folder;
    g.batchCancel = std::make_shared<std::atomic<bool>>(false);
    UpdatePoolPause();   // leave memory and CPU to the export
    SetStatus(L"");

    HWND hwnd = g.hwnd;
    std::shared_ptr<std::atomic<bool>> cancel = g.batchCancel;
    std::thread([hwnd, job, cancel]() {
        auto res = std::make_unique<BatchResult>(RunBatchExport(job, *cancel, [hwnd](int done, int total, const PathString&) {
            PostMessageW(hwnd, WM_APP_BATCH_PROGRESS, static_cast<WPARAM>(done), static_cast<LPARAM>(total));
        }));
        if (PostMessageW(hwnd, WM_APP_BATCH_DONE, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

void OnBatchDone(BatchResult* raw) {
    std::unique_ptr<BatchResult> res(raw);
    g.batchBusy = false;
    g.batchCancel.reset();
    UpdatePoolPause();

    std::wstring folderName = FileNameOf(g.batchFolder);
    if (folderName.empty()) folderName = g.batchFolder;
    std::wstring msg = res->cancelled
        ? L"Export cancelled after " + std::to_wstring(res->processed) + L" of " + std::to_wstring(g.batchTotal)
        : L"Exported " + std::to_wstring(res->exported) + (res->exported == 1 ? L" file" : L" files") + L" to " + folderName +
          L" (" + LookName(g.cfg.exportLook) + L")";
    SetStatus(msg);

    if (!res->failures.empty()) {
        std::wstring report = std::to_wstring(res->failures.size()) +
                              (res->failures.size() == 1 ? L" file couldn't be exported:\n\n" : L" files couldn't be exported:\n\n");
        size_t shown = 0;
        for (const auto& f : res->failures) {
            if (++shown > 12) { report += L"\u2026and " + std::to_wstring(res->failures.size() - 12) + L" more\n"; break; }
            report += FileNameOf(f.first) + L"\n    " + Utf8ToWide(f.second) + L"\n";
        }
        MessageBoxW(g.hwnd, report.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
}

void CancelBatch() {
    if (g.batchBusy && g.batchCancel) {
        g.batchCancel->store(true);
        SetStatus(L"Cancelling\u2026");
    }
}

// --- Fix HDRI -------------------------------------------------------------------------------

struct AnalyzeResult {
    unsigned imageId = 0;
    HdriReport report;
};

struct FixedResult {
    unsigned imageId = 0;
    bool ok = false;
    bool compare = false;            // a before/after switch, not a new fix
    bool original = false;           // (compare) the original is being shown
    std::string error;
    HdriFixResult fix;
    LoadResult load;                 // the image to show, ready for display
};

bool FixActive() { return static_cast<bool>(g.fixOriginal); }

// Fix HDRI works on colour images in the image view (not data passes or Cryptomattes).
bool CanFix() {
    return g.mode == Mode::Single && HasImage() && !ViewIsData() && !g.loadBusy && !g.layerBusy;
}

void ClearFix() {
    g.fixOriginal.reset();
    g.fixedImage.reset();
    g.showingOriginal = false;
    g.fixSaved = false;
    g.fixDiscarded = false;
    g.fixReport = HdriReport{};
    g.fixReportFor.reset();
    g.fixOptsValid = false;
}

// Before leaving an unsaved fix (another file, another layer, closing).
bool ConfirmDiscardFix() {
    if (!FixActive() || g.fixSaved || g.fixDiscarded) return true;
    const int r = MessageBoxW(g.hwnd, L"The fixed HDRI hasn't been saved.\n\nDiscard the fix?  (Ctrl+S saves it.)",
                              kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (r != IDYES) return false;
    // Kept on screen until the next image or layer has loaded (it stays if that fails).
    g.fixDiscarded = true;
    return true;
}

void ShowFixDialog();

// Toolbar Fix / H: check the image, then show the report. With a fix already
// applied, the window opens again with the same report and choices.
void StartFix() {
    if (!CanFix() || g.fixBusy) { MessageBeep(MB_OK); return; }
    // Already checked (a fix is applied, or the window was left for a Sun check): no need to check again.
    if (FixActive() || (!g.fixReportFor.expired() && g.fixReportFor.lock() == g.image)) { ShowFixDialog(); return; }
    g.fixBusy = true;
    g.fixBusyText = L"Checking the HDRI\u2026";
    HWND hwnd = g.hwnd;
    std::shared_ptr<const HdrImage> image = g.image;
    const unsigned id = g.imageId;
    std::thread([hwnd, image, id]() {
        auto res = std::make_unique<AnalyzeResult>();
        res->imageId = id;
        try { res->report = AnalyzeHdri(*image); } catch (...) { res->report = HdriReport{}; }
        if (PostMessageW(hwnd, WM_APP_ANALYZED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

void LaunchFixDisplay(std::shared_ptr<const HdrImage> source, bool compare, bool original, const HdriFixOptions& opts) {
    g.fixBusy = true;
    g.fixBusyText = compare ? std::wstring(L"Switching\u2026") : std::wstring(L"Fixing the HDRI\u2026");
    HWND hwnd = g.hwnd;
    const unsigned id = g.imageId;
    const TonemapSettings settings = g.settings;
    const HdriReport report = g.fixReport;
    std::thread([hwnd, source, id, settings, report, opts, compare, original]() {
        auto res = std::make_unique<FixedResult>();
        res->imageId = id;
        res->compare = compare;
        res->original = original;
        res->load.settings = settings;
        try {
            std::shared_ptr<HdrImage> shown;
            if (compare) {
                // Showing the original or the fixed copy again: only the display
                // is rebuilt (PrepareDisplay reads the pixels, never changes them).
                shown = std::const_pointer_cast<HdrImage>(source);
                res->ok = true;
            } else {
                shown = std::make_shared<HdrImage>();
                res->ok = FixHdri(*source, report, opts, *shown, res->fix, res->error);
            }
            if (res->ok) PrepareDisplay(res->load, std::move(shown), settings);
        } catch (const std::bad_alloc&) {
            res->ok = false;
            res->error = "Not enough memory to fix an image this large.";
        }
        if (PostMessageW(hwnd, WM_APP_FIXED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

void ToggleSunCheck(bool haveSun = false, double sunX = 0.0, double sunY = 0.0);

void OnResize();

// Fix HDRI opens as a panel over the inspector; the image stays in view.
void ShowFixDialog() {
    if (!g.fixPanel) return;
    const HdriFixOptions opts = g.fixOptsValid ? g.fixOpts : DefaultFixOptions(g.fixReport);
    FixPanelShow(g.fixPanel, FileNameOf(g.path), g.fixReport, opts);
    g.fixPanelOn = true;
    OnResize();   // places and shows the panel, and narrows the image area if needed
}

void CloseFixPanel() {
    if (!g.fixPanelOn) return;
    if (g.fixPanel) {
        if (!g.fixReportFor.expired()) { g.fixOpts = FixPanelOptions(g.fixPanel); g.fixOptsValid = true; }
        ShowWindow(g.fixPanel, SW_HIDE);
    }
    g.fixPanelOn = false;
    if (g.hwnd) OnResize();
}

// A footer button of the panel (WM_APP_FIXPANEL).
void OnFixPanel(FixWindowResult result) {
    if (!g.fixPanelOn) return;
    if (result == FixWindowResult::Cancel) { CloseFixPanel(); return; }
    if (g.fixReportFor.expired()) { CloseFixPanel(); return; }   // the image changed meanwhile
    const HdriFixOptions opts = FixPanelOptions(g.fixPanel);
    g.fixOpts = opts;
    g.fixOptsValid = true;
    if (result == FixWindowResult::SunCheck) {
        ToggleSunCheck(true, g.fixReport.sunX, g.fixReport.sunY);   // again: back to the normal view
        return;
    }
    if (g.fixBusy) { MessageBeep(MB_OK); return; }
    LaunchFixDisplay(FixActive() ? g.fixOriginal : g.image, false, false, opts);
}

void OnAnalyzed(AnalyzeResult* raw) {
    std::unique_ptr<AnalyzeResult> res(raw);
    g.fixBusy = false;
    Redraw();
    if (res->imageId != g.imageId || !CanFix()) return;   // the image changed meanwhile
    g.fixReport = res->report;
    g.fixReportFor = g.image;
    g.fixOptsValid = false;
    ShowFixDialog();
}

std::wstring FixSummary() {
    std::wstring t;
    auto add = [&](const std::wstring& s) { if (!t.empty()) t += L", "; t += s; };
    wchar_t buf[96];
    if (g.fixResult.sunRebuilt) {
        std::swprintf(buf, std::size(buf), L"sun rebuilt (%.0f)", static_cast<double>(g.fixResult.sunPeak));
        add(buf);
    }
    if (g.fixResult.repaired) add(std::to_wstring(g.fixResult.repaired) + L" broken pixel" + (g.fixResult.repaired == 1 ? L"" : L"s") + L" repaired");
    if (g.fixResult.hotRemoved) add(std::to_wstring(g.fixResult.hotRemoved) + L" hot pixel" + (g.fixResult.hotRemoved == 1 ? L"" : L"s") + L" removed");
    return t.empty() ? L"Nothing changed" : L"Fixed: " + t;
}

void OnFixed(FixedResult* raw) {
    std::unique_ptr<FixedResult> res(raw);
    g.fixBusy = false;
    if (res->imageId != g.imageId) { Redraw(); return; }   // another image or layer is showing now
    if (!res->ok) {
        Redraw();
        std::wstring msg = L"Couldn't fix the HDRI.\n\n" + Utf8ToWide(res->error);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!res->compare) {
        if (!FixActive()) g.fixOriginal = g.image;    // keep the image as loaded
        g.fixedImage = res->load.image;
        g.fixResult = res->fix;
        g.fixSaved = false;
        g.showingOriginal = false;
    } else {
        g.showingOriginal = res->original;
    }
    ++g.imageId;                                      // new pixels: stale renders are dropped
    InstallImage(res->load);
    g.pixelValid = false;
    ClampView();
    if (g.settings != res->load.settings) ApplySettings();
    if (!res->compare) SetStatus(FixSummary() + L"  \u2014  B: before / after, Ctrl+S: save");
    else SetStatus(g.showingOriginal ? L"Before: the original" : L"After: the fixed HDRI");
    Redraw();
}

void ToggleFixCompare() {
    if (!FixActive() || g.fixBusy || g.mode != Mode::Single) { MessageBeep(MB_OK); return; }
    const bool toOriginal = !g.showingOriginal;
    LaunchFixDisplay(toOriginal ? g.fixOriginal : g.fixedImage, true, toOriginal, g.fixOpts);
}

// Ctrl+S: the fixed HDRI as a new EXR (32-bit or half) or Radiance .hdr file.
void ShowSaveFixedDialog() {
    if (!FixActive() || !g.fixedImage) { SetStatus(L"Nothing to save \u2014 use Fix (H) first"); return; }
    if (g.exportBusy || g.batchBusy) { MessageBeep(MB_OK); return; }

    std::wstring stem = FileNameOf(g.path);
    if (size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    stem += L"_fixed";

    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"OpenEXR, 32-bit float (keeps every value)", L"*.exr"},
                                       {L"OpenEXR, 16-bit half float (smaller, values capped at 65,504)", L"*.exr"},
                                       {L"Radiance HDR", L"*.hdr"}};
    dlg->SetFileTypes(3, types);
    const int fmt = std::clamp(g.cfg.fixedFormat, 0, 2);
    dlg->SetFileTypeIndex(static_cast<UINT>(fmt + 1));
    dlg->SetDefaultExtension(fmt == 2 ? L"hdr" : L"exr");
    dlg->SetFileName(stem.c_str());
    dlg->SetTitle(L"Save fixed HDRI");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN);
    const std::wstring initialDir = FolderOf(g.path);
    if (!initialDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    std::wstring path;
    UINT typeIndex = static_cast<UINT>(fmt + 1);
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            item->Release();
        }
        dlg->GetFileTypeIndex(&typeIndex);
    }
    dlg->Release();
    if (path.empty()) return;

    // The extension decides between EXR and HDR; the chosen type between 32 and 16 bits.
    std::wstring lower = path;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    auto ends = [&](const wchar_t* e) { size_t n = wcslen(e); return lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0; };
    HdrSaveFormat format;
    if (ends(L".hdr")) format = HdrSaveFormat::Radiance;
    else if (ends(L".exr")) format = typeIndex == 2 ? HdrSaveFormat::ExrHalf : HdrSaveFormat::ExrFloat;
    else {
        format = typeIndex == 3 ? HdrSaveFormat::Radiance : typeIndex == 2 ? HdrSaveFormat::ExrHalf : HdrSaveFormat::ExrFloat;
        path += typeIndex == 3 ? L".hdr" : L".exr";
    }
    if (SamePath(path, g.path)) {
        MessageBoxW(g.hwnd, L"That is the original file. Please choose another name \u2014 the original is never overwritten.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (format == HdrSaveFormat::ExrHalf) {
        float peak = 0.0f;
        const float* px = g.fixedImage->rgb();
        for (size_t i = 0, n = g.fixedImage->valueCount(); i < n; ++i) peak = std::max(peak, px[i]);
        if (peak > kHalfMax) {
            wchar_t msg[400];
            std::swprintf(msg, std::size(msg),
                          L"This HDRI has values up to %.0f, but 16-bit half float stops at 65,504, so the brightest "
                          L"parts (the sun) would be capped and give less light.\n\nSave as 32-bit float instead?",
                          static_cast<double>(peak));
            const int r = MessageBoxW(g.hwnd, msg, kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return;
            if (r == IDYES) format = HdrSaveFormat::ExrFloat;
        }
    }
    g.cfg.fixedFormat = format == HdrSaveFormat::Radiance ? 2 : format == HdrSaveFormat::ExrHalf ? 1 : 0;
    SaveSettings(g.cfg);

    g.exportBusy = true;
    g.exportLookUsed = -2;   // "Saved", not "Exported"
    SetStatus(L"");
    HWND hwnd = g.hwnd;
    std::shared_ptr<const HdrImage> image = g.fixedImage;
    std::thread([hwnd, image, path, format]() {
        auto res = std::make_unique<ExportResult>();
        res->path = path;
        auto t0 = std::chrono::steady_clock::now();
        res->ok = SaveHdrImage(*image, path, format, res->error);
        res->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (PostMessageW(hwnd, WM_APP_EXPORTED, 0, reinterpret_cast<LPARAM>(res.get()))) res.release();
    }).detach();
    Redraw();
}

// --- Sun check ----------------------------------------------------------------------------------
// The quick test for a clipped sun: look straight at it with the exposure set
// so a real sun is just white. A real sun is a small white disc; a clipped
// one, far darker than a real sun, looks dark.

// 126000.4 -> "126,000"; small values keep a decimal.
std::wstring GroupedNumber(float v) {
    wchar_t buf[48];
    if (v < 100.0f) { std::swprintf(buf, std::size(buf), L"%.1f", static_cast<double>(v)); return buf; }
    std::swprintf(buf, std::size(buf), L"%.0f", static_cast<double>(v));
    std::wstring t = buf, out;
    for (size_t i = 0; i < t.size(); ++i) {
        out += t[i];
        const size_t left = t.size() - 1 - i;
        if (left > 0 && left % 3 == 0) out += L',';
    }
    return out;
}

// Brightest point of the image on screen (searched in the quick preview of big images).
bool BrightestPoint(double& x, double& y, float* lum = nullptr) {
    if (!HasImage()) return false;
    const HdrImage* src = g.preview ? g.preview.get() : g.image.get();
    const float* px = src->rgb();
    const size_t n = static_cast<size_t>(src->width) * src->height;
    size_t best = 0;
    float bestL = -1.0f;
    for (size_t i = 0; i < n; ++i) {
        const float* p = px + i * 3;
        const float L = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (L > bestL && std::isfinite(L)) { bestL = L; best = i; }
    }
    const double sx = static_cast<double>(g.image->width) / src->width;
    const double sy = static_cast<double>(g.image->height) / src->height;
    if (lum) *lum = bestL;
    x = (static_cast<double>(best % src->width) + 0.5) * sx;
    y = (static_cast<double>(best / src->width) + 0.5) * sy;
    return true;
}

void ToggleSunCheck(bool haveSun, double sunX, double sunY) {
    if (g.sunCheck.on) {   // back to how it was
        g.sunCheck.on = false;
        g.panoOn = g.sunCheck.panoOn;
        g.pano = g.sunCheck.pano;
        g.fit = g.sunCheck.fit;
        g.zoom = g.sunCheck.zoom;
        g.cx = g.sunCheck.cx;
        g.cy = g.sunCheck.cy;
        if (g.fit) SetFit(); else ClampView();
        g.panoKey = App::PanoKey{};
        SetSettings(g.sunCheck.look);
        RequestFullRender();
        SetStatus(L"Sun check off");
        return;
    }
    if (g.mode != Mode::Single || !HasImage() || ViewIsData() || g.loadBusy) { MessageBeep(MB_OK); return; }
    double x = sunX, y = sunY;
    // The analysis, if it describes what is on screen (the original or its fixed copy).
    const bool known = !g.fixReportFor.expired() &&
                       (g.fixReportFor.lock() == g.image || (FixActive() && g.fixReportFor.lock() == g.fixOriginal));
    const HdriReport& r = g.fixReport;
    if (!haveSun) {
        if (known && r.sunSizeDeg > 0.0f) { x = r.sunX; y = r.sunY; }
        else if (!BrightestPoint(x, y)) return;
    }
    // Exposure: the sun's real brightness is shown as just white. Then a real
    // sun is a small white disc, and a clipped one, far below that, looks dark.
    float brightest = 0.0f;
    if (known) brightest = r.maxLum;
    else { double bx, by; BrightestPoint(bx, by, &brightest); }
    double target = std::max(1.0f, brightest);
    std::wstring status;
    const bool clippedSun = known && r.sun == HdriReport::Sun::Rebuildable;
    if (clippedSun && FixActive() && g.image == g.fixedImage && g.fixResult.sunRebuilt) {
        target = g.fixResult.sunPeak;
        status = L"Sun check: clipped sun " + GroupedNumber(r.clipLevel) + L" → rebuilt " + GroupedNumber(g.fixResult.sunPeak) +
                 L", shown just white. B compares with the original. S goes back.";
    } else if (clippedSun) {
        const HdriFixOptions o = g.fixOptsValid ? g.fixOpts : DefaultFixOptions(r);
        target = SunPeakAt(r, SkyEV(o.sky) + o.sunStrengthEV, o.sunSizeDeg);
        status = L"Sun check: the sun is flat at " + GroupedNumber(r.clipLevel) + L"; a real sun here would be about " +
                 GroupedNumber(static_cast<float>(target)) + L". Shown so a real sun is just white — this one is dark: clipped (H fixes it). S goes back.";
    } else {
        status = L"Sun check: brightest " + GroupedNumber(brightest) + L", shown just white. A real sun is a small sharp disc; "
                 L"a wide flat blob with a hard edge is clipped. S goes back.";
    }
    const float ev = static_cast<float>(std::clamp(-std::log2(target), static_cast<double>(kSunCheckMinEV), 0.0));
    g.sunCheck.on = true;
    g.sunCheck.look = g.settings;
    g.sunCheck.panoOn = g.panoOn;
    g.sunCheck.pano = g.pano;
    g.sunCheck.fit = g.fit;
    g.sunCheck.zoom = g.zoom;
    g.sunCheck.cx = g.cx;
    g.sunCheck.cy = g.cy;
    if (LooksPanoramic(g.image->width, g.image->height)) {
        g.panoOn = true;
        g.panoFast = false;
        g.pano.yaw = (x / g.image->width - 0.5) * 360.0;
        g.pano.pitch = (0.5 - y / g.image->height) * 180.0;
        g.pano.fov = 30.0;
        NormalizePanoView(g.pano);
    } else {
        g.fit = false;
        g.zoom = std::min(kMaxZoom, std::max(1.0, FitZoom() * 6.0));
        g.cx = x;
        g.cy = y;
        ClampView();
    }
    g.panoKey = App::PanoKey{};
    g.pixelValid = false;
    SetSettings(TonemapSettings{ev, Tonemap::Clip});
    RequestFullRender();
    SetStatus(status);
    Redraw();
}

// --- browsing the folder -------------------------------------------------------------------

struct NavInfo { int index = -1; bool exact = false; int count = 0; };

NavInfo CurrentNav() {
    NavInfo n;
    const std::wstring& target = TargetPath();
    if (target.empty()) return n;
    n.count = static_cast<int>(g.siblings.size());
    n.index = FindFileIndex(g.siblings, target, n.exact);
    return n;
}

bool CanGoPrev() { NavInfo n = CurrentNav(); return n.index > 0; }
bool CanGoNext() {
    NavInfo n = CurrentNav();
    if (n.index < 0) return false;
    return n.exact ? n.index + 1 < n.count : n.index < n.count;
}

enum class Nav { Prev, Next, First, Last };

void Navigate(Nav where) {
    const std::wstring target = TargetPath();
    if (target.empty()) return;
    RefreshSiblings(target);   // pick up files added or removed since last time
    NavInfo n = CurrentNav();
    if (n.count == 0) return;

    int i = -1;
    switch (where) {
        case Nav::Prev:  i = n.index - 1; break;
        case Nav::Next:  i = n.exact ? n.index + 1 : n.index; break;
        case Nav::First: i = 0; break;
        case Nav::Last:  i = n.count - 1; break;
    }
    if (i < 0 || i >= n.count) { MessageBeep(MB_OK); return; }
    if (n.exact && i == n.index) return;
    StartLoad(g.siblings[static_cast<size_t>(i)]);
}


// --- folder grid ------------------------------------------------------------------------------

void SetGridTitle() {
    if (g.searchActive) { SetWindowTextW(g.hwnd, TitleFor(L"Search: " + g.searchQuery).c_str()); return; }
    std::wstring name = FileNameOf(g.gridFolder);
    if (name.empty()) name = g.gridFolder;
    SetWindowTextW(g.hwnd, TitleFor(name).c_str());
}

int GridIndexOf(const std::wstring& path) {
    for (size_t i = 0; i < g.items.size(); ++i)
        if (SamePath(g.items[i].path, path)) return static_cast<int>(i);
    return -1;
}

void LayoutGrid() { g.grid.Layout(Viewport(), static_cast<int>(g.items.size())); }

bool ShiftDown() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

// Selection: one or more "picked" tiles; g.selected is the focused tile and
// g.anchor where a Shift range starts.
void PickOnly(int i) {
    for (auto& it : g.items) it.picked = false;
    if (i >= 0 && i < static_cast<int>(g.items.size())) g.items[static_cast<size_t>(i)].picked = true;
    g.anchor = i;
}

void PickRange(int a, int b) {
    for (auto& it : g.items) it.picked = false;
    if (a < 0) a = b;
    for (int k = std::min(a, b); k <= std::max(a, b); ++k)
        if (k >= 0 && k < static_cast<int>(g.items.size())) g.items[static_cast<size_t>(k)].picked = true;
}

// Moves the focus to tile i; `extend` selects the range from the anchor.
void SelectItem(int i, bool extend = false) {
    if (g.items.empty()) return;
    g.selected = std::clamp(i, 0, static_cast<int>(g.items.size()) - 1);
    if (extend) PickRange(g.anchor, g.selected);
    else PickOnly(g.selected);
    LayoutGrid();
    g.grid.EnsureVisible(g.selected);
    Redraw();
}

void PickAll() {
    for (auto& it : g.items) it.picked = true;
    Redraw();
}

// Size and date of one file (for search results, which come from different folders).
FileEntry EntryFor(const std::wstring& path) {
    FileEntry e;
    e.path = path;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        e.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        e.writeTime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    return e;
}

// The scan of the library folder that contains `folder` (null if none, or not scanned yet).
std::wstring PathKey(const std::wstring& path) {
    std::wstring k = NormalizeFolderPath(path);
    for (auto& c : k) c = static_cast<wchar_t>(std::towlower(c));
    return k;
}

const ScanResult* ScanOf(const std::wstring& libraryFolder) {
    auto it = g.scans.find(PathKey(libraryFolder));
    return it == g.scans.end() ? nullptr : it->second.get();
}

// Scans in library order (null while a folder is being scanned).
std::vector<const ScanResult*> LibraryScans() {
    std::vector<const ScanResult*> v;
    for (const auto& f : g.library.Folders()) v.push_back(ScanOf(f.path));
    return v;
}

const ScanResult* ScanFor(const std::wstring& folder) {
    const int root = g.library.RootOf(folder);
    if (root < 0) return nullptr;
    const ScanResult* sc = ScanOf(g.library.Folders()[static_cast<size_t>(root)].path);
    return sc && !sc->missing ? sc : nullptr;
}

// "Include subfolders" belongs to the library folder a folder is in.
bool SubfoldersFor(const std::wstring& folder) {
    const int root = g.library.RootOf(folder);
    return root >= 0 && g.library.Folders()[static_cast<size_t>(root)].subfolders;
}

// The files of `folder`, read now (sizes and dates fresh); with its
// subfolders, in the order of the folder tree.
std::vector<FileEntry> ListFolderEntries(const std::wstring& folder, bool subfolders) {
    std::vector<FileEntry> entries = ListHdrFileEntries(folder);
    if (!subfolders) return entries;
    const ScanResult* sc = ScanFor(folder);
    const int start = sc ? sc->Find(folder) : -1;
    if (start < 0) return entries;
    std::vector<int> stack(sc->folders[static_cast<size_t>(start)].children.rbegin(),
                           sc->folders[static_cast<size_t>(start)].children.rend());
    while (!stack.empty() && entries.size() < 20000) {
        const ScannedFolder& f = sc->folders[static_cast<size_t>(stack.back())];
        stack.pop_back();
        for (auto& e : ListHdrFileEntries(f.path)) entries.push_back(std::move(e));
        for (auto c = f.children.rbegin(); c != f.children.rend(); ++c) stack.push_back(*c);
    }
    return entries;
}

// Shows `entries` in the grid. Thumbnails already made for unchanged files
// are kept (when `sameListing`, i.e. a refresh); new or changed files are
// queued. `select` picks the tile to highlight.
void ShowEntries(std::vector<FileEntry> entries, bool sameListing, const std::wstring& select) {
    const bool sameFolder = sameListing;
    const std::wstring keepSelection = (g.selected >= 0 && g.selected < static_cast<int>(g.items.size()))
                                       ? g.items[static_cast<size_t>(g.selected)].path : std::wstring();

    std::vector<GridItem> fresh;
    fresh.reserve(entries.size());
    std::vector<unsigned char> done(entries.size(), 0);
    bool unchanged = sameFolder && entries.size() == g.items.size();

    for (size_t i = 0; i < entries.size(); ++i) {
        const FileEntry& e = entries[i];
        GridItem item;
        bool reused = false;
        if (sameFolder) {
            int old = GridIndexOf(e.path);
            if (old >= 0) {
                GridItem& o = g.items[static_cast<size_t>(old)];
                if (o.fileSize == e.size && o.writeTime == e.writeTime) {
                    item = std::move(o);
                    reused = true;
                    if (static_cast<size_t>(old) != i) unchanged = false;
                }
            }
        }
        if (!reused) {
            unchanged = false;
            item.path = e.path;
            item.name = FileNameOf(e.path);
            item.fileSize = e.size;
            item.writeTime = e.writeTime;
        }
        done[i] = item.state != GridItem::State::Waiting;
        fresh.push_back(std::move(item));
    }

    if (!unchanged) {
        // Unchanged listings keep their running jobs; otherwise start a new list.
        g.items = std::move(fresh);
        ++g.gridGen;
        std::vector<PathString> paths;
        paths.reserve(g.items.size());
        for (const auto& it : g.items) paths.push_back(it.path);
        if (g.pool) g.pool->SetJobs(g.gridGen, std::move(paths), g.grid.BoxWidth(), g.grid.BoxHeight(), done);
        g.jobBoxW = g.grid.BoxWidth();
        g.jobBoxH = g.grid.BoxHeight();
    } else {
        for (size_t i = 0; i < fresh.size(); ++i) g.items[i] = std::move(fresh[i]);
    }

    if (!sameFolder) g.grid.ScrollTo(0);
    g.mode = Mode::Grid;
    g.gridHover = -1;
    KillTimer(g.hwnd, kRenderTimer);
    CloseFixPanel();

    int sel = select.empty() ? -1 : GridIndexOf(select);
    if (sel < 0 && sameFolder && !keepSelection.empty()) sel = GridIndexOf(keepSelection);
    if (sel < 0) sel = 0;
    g.selected = g.items.empty() ? -1 : sel;
    // Coming back from an image, or a new folder: just that tile is selected.
    // Refreshing the same listing keeps a multi-selection.
    bool anyPicked = false;
    for (const auto& it : g.items) anyPicked |= it.picked;
    if (!select.empty() || !sameFolder || !anyPicked) PickOnly(g.selected);
    LayoutGrid();
    if (g.selected >= 0) g.grid.EnsureVisible(g.selected);
    SetGridTitle();
    Redraw();
}

void EndSearch(bool reopen);
void RefreshTree();
void SaveLibrary();

// Shows the grid for `folder` (with its subfolders when its library folder says so).
void OpenFolder(std::wstring folder, const std::wstring& select) {
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    if (folder.size() == 3 && folder[1] == L':') folder.pop_back();   // "D:\" -> "D:"

    if (g.searchActive) EndSearch(false);
    const bool sub = SubfoldersFor(folder);
    const bool sameFolder = !g.gridFolder.empty() && SamePath(folder, g.gridFolder) && sub == g.gridSubfolders;
    g.gridFolder = folder;
    g.gridSubfolders = sub;
    ShowEntries(ListFolderEntries(folder, sub), sameFolder, select);
    if (!SamePath(g.cfg.lastFolder, folder)) { g.cfg.lastFolder = folder; SaveLibrary(); }
    RefreshTree();
}

// From the single image to the grid of its folder.
void ShowGrid() {
    const std::wstring target = TargetPath();
    if (g.mode == Mode::Grid) return;
    if (g.searchActive && !g.items.empty()) {   // back to the search results
        g.mode = Mode::Grid;
        CloseFixPanel();
        const int i = GridIndexOf(target);
        if (i >= 0) { g.selected = i; PickOnly(i); }
        LayoutGrid();
        if (g.selected >= 0) g.grid.EnsureVisible(g.selected);
        SetGridTitle();
        Redraw();
        return;
    }
    if (target.empty()) {
        if (!g.gridFolder.empty()) OpenFolder(g.gridFolder, L"");
        return;
    }
    OpenFolder(FolderOf(target), target);
}

// From the grid to the selected image.
void OpenSelected() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.items.size())) return;
    const GridItem& it = g.items[static_cast<size_t>(g.selected)];
    if (it.state == GridItem::State::Failed) {
        std::wstring msg = it.name + L"\n\n" + Utf8ToWide(it.error);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    const bool alreadyShown = HasImage() && SamePath(it.path, TargetPath()) && !g.loadBusy;
    if (!alreadyShown && !ConfirmDiscardFix()) return;   // ask before switching views
    g.mode = Mode::Single;
    g.drag = Drag::None;
    if (alreadyShown) {
        SetWindowTextW(g.hwnd, TitleFor(FileNameOf(g.path)).c_str());
        SetFit();
        ApplySettings();
    } else {
        SetWindowTextW(g.hwnd, TitleFor(it.name).c_str());
        StartLoad(it.path);
    }
    Redraw();
}

void OnThumb(ThumbResult* raw) {
    std::unique_ptr<ThumbResult> res(raw);
    if (res->generation != g.gridGen || res->index < 0 || res->index >= static_cast<int>(g.items.size())) return;
    GridItem& it = g.items[static_cast<size_t>(res->index)];
    if (!res->ok && it.state == GridItem::State::Done) return;   // keep the smaller one we had
    it.state = res->ok ? GridItem::State::Done : GridItem::State::Failed;
    it.imageWidth = res->imageWidth;
    it.imageHeight = res->imageHeight;
    it.format = res->format;
    it.error = res->error;
    it.thumb = std::move(res->thumb);
    it.peakLum = res->ok ? res->peakLum : -1.0f;
    it.rangeStops = res->rangeStops;
    it.statsReduced = res->statsReduced;
    it.bmp.reset();
    if (SamePath(g.inspBmpPath, it.path)) g.inspBmpPath.clear();   // a sharper thumbnail for the inspector too
    if (res->index == g.selected && InspectorShown()) { RECT ir = InspectorRect(); InvalidateRect(g.hwnd, &ir, FALSE); }
    if (g.mode == Mode::Grid) {
        RECT r = g.grid.ItemRect(res->index);
        RECT vp = Viewport();
        RECT bar = ClientRect(); bar.top = vp.bottom;
        InvalidateRect(g.hwnd, &r, FALSE);
        InvalidateRect(g.hwnd, &bar, FALSE);   // "N left" counter
    } else if (g.loadBusy && SamePath(it.path, TargetPath())) {
        Redraw();   // can now show it as a placeholder
    }
}

// After the grid got bigger: re-make thumbnails that would look soft at the
// new size (the old ones are shown stretched meanwhile).
void RemakeThumbsIfNeeded() {
    KillTimer(g.hwnd, kThumbSizeTimer);
    if (!g.pool || g.items.empty()) return;
    const int bw = g.grid.BoxWidth(), bh = g.grid.BoxHeight();
    if (bw <= g.jobBoxW && bh <= g.jobBoxH) return;   // smaller: shrinking stays sharp

    std::vector<unsigned char> done(g.items.size(), 0);
    bool any = false;
    for (size_t i = 0; i < g.items.size(); ++i) {
        const GridItem& it = g.items[i];
        if (it.state == GridItem::State::Failed) { done[i] = 1; continue; }
        if (it.state == GridItem::State::Done) {
            int dw, dh;
            ThumbFitSize(it.imageWidth, it.imageHeight, bw, bh, dw, dh);
            done[i] = (it.thumb.width + 1 >= dw && it.thumb.height + 1 >= dh) ? 1 : 0;
        }
        any |= done[i] == 0;
    }
    g.jobBoxW = bw;
    g.jobBoxH = bh;
    if (!any) return;
    ++g.gridGen;
    std::vector<PathString> paths;
    paths.reserve(g.items.size());
    for (const auto& it : g.items) paths.push_back(it.path);
    g.pool->SetJobs(g.gridGen, std::move(paths), bw, bh, done);
    Redraw();
}

void SetThumbScale(float scale) {
    scale = std::clamp(std::round(scale * 20.0f) / 20.0f, 0.5f, 2.0f);   // 5% steps
    if (std::fabs(scale - g.thumbScale) < 1e-4f) return;
    g.thumbScale = scale;
    g.grid.SetDpi(g.dpi, scale);
    LayoutGrid();
    if (g.selected >= 0) g.grid.EnsureVisible(g.selected);
    SetTimer(g.hwnd, kThumbSizeTimer, 400, nullptr);
    Redraw();
}

void ShowOpenFolderDialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Open a folder of HDR / EXR images");
    std::wstring start = !g.gridFolder.empty() ? g.gridFolder : FolderOf(TargetPath());
    if (!start.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                OpenFolder(path, L"");
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

bool IsFolder(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// A path from the command line or a drop: folders open the grid, files the image.
void OpenPath(const std::wstring& path) {
    if (IsFolder(path)) {
        OpenFolder(path, L"");
    } else {
        g.mode = Mode::Single;
        StartLoad(path);
    }
}

// --- folder library (sidebar) -------------------------------------------------------------

void SaveLibrary() {
    g.cfg.libraryFolders = g.library.Save();
    g.cfg.expandedFolders.assign(g.expanded.begin(), g.expanded.end());
    SaveSettings(g.cfg);
}

bool IsExpanded(const std::wstring& path) { return g.expanded.count(PathKey(path)) > 0; }

void RunSearch();
void InvalidateInfoBar();

// Scans a library folder in the background (subfolders and file names).
struct ScanDone {
    std::wstring root;
    ScanResult result;
};

void StartScan(const std::wstring& root) {
    HWND hwnd = g.hwnd;
    std::thread([hwnd, root]() {
        auto r = std::make_unique<ScanDone>();
        r->root = root;
        r->result = ScanFolderTree(root);
        if (PostMessageW(hwnd, WM_APP_SCANNED, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
    }).detach();
}

void ScanAll() {
    for (const auto& f : g.library.Folders()) StartScan(f.path);
}

// Watches the library folders for new, changed and removed files (one
// thread for all, woken up when the list of folders changes).
struct Watcher {
    std::mutex mu;
    std::vector<std::wstring> roots;
    HANDLE wake = nullptr;
} watcher;

void WatcherLoop(HWND hwnd) {
    for (;;) {
        std::vector<std::wstring> roots;
        { std::lock_guard<std::mutex> lock(watcher.mu); roots = watcher.roots; }
        std::vector<HANDLE> hs{watcher.wake};
        for (const auto& r : roots) {
            if (hs.size() >= MAXIMUM_WAIT_OBJECTS) break;
            std::wstring p = r;
            if (p.size() == 2 && p[1] == L':') p += L'\\';
            HANDLE h = FindFirstChangeNotificationW(p.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                                     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (h != INVALID_HANDLE_VALUE) hs.push_back(h);
        }
        for (;;) {
            const DWORD w = WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(), FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;   // the list of folders changed
            if (w > WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + hs.size()) {
                PostMessageW(hwnd, WM_APP_LIBCHANGED, 0, 0);
                Sleep(300);   // a copy of many files: one message per moment is enough
                if (!FindNextChangeNotification(hs[w - WAIT_OBJECT_0])) break;
            } else {
                Sleep(1000);
                break;
            }
        }
        for (size_t i = 1; i < hs.size(); ++i) FindCloseChangeNotification(hs[i]);
    }
}

void RestartWatcher() {
    {
        std::lock_guard<std::mutex> lock(watcher.mu);
        watcher.roots.clear();
        for (const auto& f : g.library.Folders()) watcher.roots.push_back(f.path);
    }
    if (!watcher.wake) {
        watcher.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!watcher.wake) return;
        std::thread(WatcherLoop, g.hwnd).detach();
    } else {
        SetEvent(watcher.wake);
    }
}

// Rebuilds the tree rows (and the search matches per row) and redraws the sidebar.
void RefreshTree() {
    g.rows = BuildTreeRows(g.library, LibraryScans(), IsExpanded);
    g.rowMatches.clear();
    if (g.searchActive) {
        g.rowMatches.assign(g.rows.size(), 0);
        for (size_t r = 0; r < g.rows.size(); ++r)
            for (const auto& hp : g.searchHitPaths)
                if (IsInsideFolder(hp, g.rows[r].path)) ++g.rowMatches[r];
    }
    g.sidebar.Layout(SidebarRect(), g.dpi, static_cast<int>(g.rows.size()));
    if (g.hwnd) { RECT r = SidebarRect(); InvalidateRect(g.hwnd, &r, FALSE); }
}

int CurrentRow() {
    const std::wstring folder = g.mode == Mode::Grid ? (g.searchActive ? std::wstring() : g.gridFolder) : FolderOf(TargetPath());
    if (folder.empty()) return -1;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (SamePathNoCase(g.rows[i].path, folder)) return static_cast<int>(i);
    return -1;
}

std::wstring LibraryStatus() {
    if (g.library.Size() == 0) return {};
    int files = 0, scanning = 0, missing = 0;
    for (const auto& f : g.library.Folders()) {
        const ScanResult* sc = ScanOf(f.path);
        if (!sc) ++scanning;
        else if (sc->missing) ++missing;
        else if (!sc->folders.empty()) files += sc->folders[0].total;
    }
    wchar_t buf[160];
    const int n = static_cast<int>(g.library.Size());
    std::swprintf(buf, std::size(buf), L"%d %ls · %d %ls", files, files == 1 ? L"file" : L"files", n, n == 1 ? L"folder" : L"folders");
    std::wstring t = buf;
    if (scanning) t = L"Reading folders…  " + t;
    if (missing) t += L" · " + std::to_wstring(missing) + L" not found";
    return t;
}

void OnScanned(ScanDone* raw) {
    std::unique_ptr<ScanDone> res(raw);
    if (g.library.IndexOf(res->root) < 0) return;   // removed meanwhile
    g.scans[PathKey(res->root)] = std::make_unique<ScanResult>(std::move(res->result));
    RefreshTree();
    if (g.searchActive) RunSearch();
    // A grid that includes subfolders needed this scan to list them.
    else if (g.mode == Mode::Grid && g.gridSubfolders && IsInsideFolder(g.gridFolder, res->root) && g.drag == Drag::None)
        OpenFolder(g.gridFolder, L"");
    InvalidateInfoBar();
}

// Something changed on disk in a library folder: rescan, and refresh the grid if it shows a folder.
void OnLibraryChanged() {
    KillTimer(g.hwnd, kLibraryTimer);
    ScanAll();
    if (g.mode == Mode::Grid && !g.searchActive && !g.gridFolder.empty() && !g.batchBusy && g.drag == Drag::None)
        OpenFolder(g.gridFolder, L"");
}

// Folders that were missing (an unplugged drive) are looked for again now and then.
void CheckMissingFolders() {
    bool back = false;
    for (const auto& f : g.library.Folders()) {
        const ScanResult* sc = ScanOf(f.path);
        if (!sc || !sc->missing) continue;
        std::wstring p = f.path;
        if (p.size() == 2 && p[1] == L':') p += L'\\';
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { StartScan(f.path); back = true; }
    }
    if (back) RestartWatcher();
}

void AddLibraryFolder(const std::wstring& path) {
    const size_t before = g.library.Size();
    const int i = g.library.Add(path);
    if (i < 0) return;
    const std::wstring& added = g.library.Folders()[static_cast<size_t>(i)].path;
    if (g.library.Size() > before) {
        g.expanded.insert(PathKey(added));
        SaveLibrary();
        StartScan(added);
        RestartWatcher();
        SetStatus(L"Added " + LeafName(added) + L" to the library");
    }
    RefreshTree();
    OpenFolder(added, L"");
}

std::wstring PickFolder(const wchar_t* title) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

void ShowAddFolderDialog() {
    const std::wstring path = PickFolder(L"Add a folder to the EXRay library");
    if (!path.empty()) AddLibraryFolder(path);
}

// Takes a folder off the list. The folder and its files stay on the disk.
void RemoveLibraryFolder(int index) {
    if (index < 0 || index >= static_cast<int>(g.library.Size())) return;
    const std::wstring path = g.library.Folders()[static_cast<size_t>(index)].path;
    const std::wstring msg = L"Remove “" + path + L"” from EXRay?\n\nOnly the link in EXRay is removed: the folder "
                             L"and its files stay on your disk. Ctrl+Z brings it back.";
    if (MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    g.library.Remove(static_cast<size_t>(index));
    g.scans.erase(PathKey(path));
    SaveLibrary();
    RestartWatcher();
    RefreshTree();
    if (g.searchActive) RunSearch();
    SetStatus(L"Removed " + LeafName(path) + L" from the library — Ctrl+Z brings it back");
}

void UndoRemoveFolder() {
    if (!g.library.CanUndo()) { MessageBeep(MB_OK); return; }
    const std::wstring path = g.library.UndoPath();
    if (!g.library.Undo()) { MessageBeep(MB_OK); return; }
    SaveLibrary();
    StartScan(path);
    RestartWatcher();
    RefreshTree();
    SetStatus(LeafName(path) + L" is back in the library");
}

void ToggleExpanded(const std::wstring& path) {
    const std::wstring k = PathKey(path);
    if (!g.expanded.erase(k)) g.expanded.insert(k);
    SaveLibrary();
    RefreshTree();
}

void SetLibrarySubfolders(int index, bool on) {
    if (index < 0 || index >= static_cast<int>(g.library.Size())) return;
    g.library.SetSubfolders(static_cast<size_t>(index), on);
    SaveLibrary();
    const std::wstring root = g.library.Folders()[static_cast<size_t>(index)].path;
    if (g.mode == Mode::Grid && !g.searchActive && IsInsideFolder(g.gridFolder, root)) OpenFolder(g.gridFolder, L"");
}

void ShowInExplorer(const std::wstring& path, bool select) {
    if (path.empty()) return;
    const std::wstring args = select ? L"/select,\"" + path + L"\"" : L"\"" + path + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// The right-click menu of a folder in the sidebar.
void ShowFolderMenu(int row, POINT client) {
    if (row < 0 || row >= static_cast<int>(g.rows.size())) return;
    const TreeRow r = g.rows[static_cast<size_t>(row)];
    const bool isRoot = r.depth == 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (r.missing ? MF_GRAYED : 0), IDM_FOLDER_OPEN, L"&Open");
    AppendMenuW(m, MF_STRING | (r.missing ? MF_GRAYED : 0), IDM_FOLDER_EXPLORER, L"Show in &Explorer");
    AppendMenuW(m, MF_STRING, IDM_FOLDER_RESCAN, L"&Rescan now");
    if (isRoot) {
        const bool sub = g.library.Folders()[static_cast<size_t>(r.root)].subfolders;
        AppendMenuW(m, MF_STRING | (sub ? MF_CHECKED : 0), IDM_FOLDER_SUB, L"Include &subfolders");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_FOLDER_REMOVE, L"Re&move from EXRay…  (files stay on disk)");
    }
    POINT pt = client;
    ClientToScreen(g.hwnd, &pt);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case IDM_FOLDER_OPEN: OpenFolder(r.path, L""); break;
        case IDM_FOLDER_EXPLORER: ShowInExplorer(r.path, false); break;
        case IDM_FOLDER_RESCAN: StartScan(g.library.Folders()[static_cast<size_t>(r.root)].path); break;
        case IDM_FOLDER_SUB: SetLibrarySubfolders(r.root, !g.library.Folders()[static_cast<size_t>(r.root)].subfolders); break;
        case IDM_FOLDER_REMOVE: RemoveLibraryFolder(r.root); break;
    }
}

// --- search ---------------------------------------------------------------------------------

std::wstring SearchText() {
    if (!g.searchEdit) return {};
    const int n = GetWindowTextLengthW(g.searchEdit);
    std::wstring t(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(g.searchEdit, t.data(), n + 1);
    t.resize(static_cast<size_t>(n));
    const size_t a = t.find_first_not_of(L" \t"), b = t.find_last_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : t.substr(a, b - a + 1);
}

// Searches file names in the library (or the folder that was showing) and shows the matches in the grid.
void RunSearch() {
    KillTimer(g.hwnd, kSearchTimer);
    const std::wstring q = SearchText();
    if (q.empty()) { EndSearch(true); return; }
    if (!g.searchActive) g.searchFolder = g.mode == Mode::Grid ? g.gridFolder : FolderOf(TargetPath());

    std::vector<const ScanResult*> scans = LibraryScans();
    std::unique_ptr<ScanResult> local;
    std::wstring within = (!g.searchAll && !g.searchFolder.empty()) ? g.searchFolder : std::wstring();
    if ((!within.empty() && !ScanFor(within)) || (within.empty() && g.library.Size() == 0 && !g.searchFolder.empty())) {
        // A folder that isn't in the library: read it (and its subfolders) now.
        if (within.empty()) within = g.searchFolder;
        local = std::make_unique<ScanResult>(ScanFolderTree(within, 2000, 6));
        scans = {local.get()};
    }
    const std::vector<SearchHit> hits = SearchLibrary(scans, q, within, 5000);
    std::vector<FileEntry> entries;
    entries.reserve(hits.size());
    g.searchHitPaths.clear();
    for (const auto& h : hits) { entries.push_back(EntryFor(h.path)); g.searchHitPaths.push_back(h.path); }
    const bool same = g.searchActive;
    g.searchActive = true;
    g.searchQuery = q;
    g.searchHits = hits.size();
    // Thumbnails already made (in the folder grid or the last search) are reused.
    if (!same) for (auto& it : g.items) it.picked = false;   // a new search starts with one selected
    ShowEntries(std::move(entries), true, L"");
    if (!same) { g.grid.ScrollTo(0); Redraw(); }
    RefreshTree();
}

void EndSearch(bool reopen) {
    KillTimer(g.hwnd, kSearchTimer);
    if (!g.searchActive) return;
    g.searchActive = false;
    g.searchQuery.clear();
    g.searchHitPaths.clear();
    if (g.searchEdit && GetWindowTextLengthW(g.searchEdit) > 0) SetWindowTextW(g.searchEdit, L"");
    KillTimer(g.hwnd, kSearchTimer);   // SetWindowText above started it again
    const std::wstring back = g.searchFolder;
    g.searchFolder.clear();
    if (reopen && !back.empty()) OpenFolder(back, L"");
    else if (reopen) { g.items.clear(); ++g.gridGen; g.selected = -1; SetGridTitle(); Redraw(); }
    RefreshTree();
}

void FocusSearch() {
    if (!g.searchEdit) return;
    if (!SidebarShown()) { g.cfg.showSidebar = true; OnResize(); }
    if (!SidebarShown()) { MessageBeep(MB_OK); return; }   // the window is too narrow for the sidebar
    SetFocus(g.searchEdit);
    SendMessageW(g.searchEdit, EM_SETSEL, 0, -1);
}

LRESULT CALLBACK SearchEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { SetWindowTextW(h, L""); EndSearch(true); SetFocus(g.hwnd); return 0; }
        if (wp == VK_RETURN || wp == VK_DOWN) { RunSearch(); SetFocus(g.hwnd); return 0; }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;   // no beep
    return CallWindowProcW(g.searchEditProc, h, msg, wp, lp);
}

// --- drawing ---------------------------------------------------------------------------------

void DrawCenteredText(HDC dc, const RECT& area, const std::wstring& line1, const std::wstring& line2) {
    SetBkMode(dc, TRANSPARENT);
    RECT r1 = area, r2 = area;
    int mid = (area.top + area.bottom) / 2;
    r1.bottom = mid; r1.top = mid - Scale(30);
    r2.top = mid + Scale(4); r2.bottom = mid + Scale(30);

    SelectObject(dc, g.bigFont);
    SetTextColor(dc, kTextColor);
    DrawTextW(dc, line1.c_str(), -1, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (!line2.empty()) {
        SelectObject(dc, g.font);
        SetTextColor(dc, kDimText);
        DrawTextW(dc, line2.c_str(), -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
}

// Draws the visible part of `bmp` (which shows the whole image at its own,
// usually smaller, resolution) into the viewport for the current zoom & pan.
void DrawBitmapView(HDC dc, HDC memDC, const Bitmap& bmp, const RECT& vp) {
    const double W = g.image->width, H = g.image->height;
    const double sx = bmp.width() / W, sy = bmp.height() / H;
    const double vcx = (vp.left + vp.right) / 2.0, vcy = (vp.top + vp.bottom) / 2.0;
    const double halfW = (vp.right - vp.left) / (2.0 * g.zoom), halfH = (vp.bottom - vp.top) / (2.0 * g.zoom);

    const double ix0 = std::max(0.0, g.cx - halfW), ix1 = std::min(W, g.cx + halfW);
    const double iy0 = std::max(0.0, g.cy - halfH), iy1 = std::min(H, g.cy + halfH);
    if (ix1 <= ix0 || iy1 <= iy0) return;

    // Whole source pixels covering the visible area; the clip rect trims the overhang.
    const int bx0 = std::max(0, static_cast<int>(std::floor(ix0 * sx)));
    const int bx1 = std::min(bmp.width(), static_cast<int>(std::ceil(ix1 * sx)));
    const int by0 = std::max(0, static_cast<int>(std::floor(iy0 * sy)));
    const int by1 = std::min(bmp.height(), static_cast<int>(std::ceil(iy1 * sy)));
    if (bx1 <= bx0 || by1 <= by0) return;

    const int dx0 = static_cast<int>(std::lround(vcx + (bx0 / sx - g.cx) * g.zoom));
    const int dx1 = static_cast<int>(std::lround(vcx + (bx1 / sx - g.cx) * g.zoom));
    const int dy0 = static_cast<int>(std::lround(vcy + (by0 / sy - g.cy) * g.zoom));
    const int dy1 = static_cast<int>(std::lround(vcy + (by1 / sy - g.cy) * g.zoom));
    if (dx1 <= dx0 || dy1 <= dy0) return;

    const bool shrinking = (dx1 - dx0) < (bx1 - bx0) || (dy1 - dy0) < (by1 - by0);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, vp.left, vp.top, vp.right, vp.bottom);
    HGDIOBJ old = SelectObject(memDC, bmp.handle());
    SetStretchBltMode(dc, shrinking ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, dx0, dy0, dx1 - dx0, dy1 - dy0, memDC, bx0, by0, bx1 - bx0, by1 - by0, SRCCOPY);
    SelectObject(memDC, old);
    RestoreDC(dc, saved);
}

bool FullIsCurrent() { return !g.levels.empty() && g.levelsSettings == g.settings; }

// Renders the 360 view from one or more copies of the panorama (largest first).
void DrawPano(HDC dc, const RECT& vp, const std::vector<const Bitmap*>& sources) {
    const int vw = vp.right - vp.left, vh = vp.bottom - vp.top;
    if (vw <= 0 || vh <= 0 || sources.empty()) return;
    const int div = g.panoFast ? 2 : 1;
    const int rw = std::max(1, vw / div), rh = std::max(1, vh / div);
    try {
        if (!g.panoBmp || g.panoBmp->width() != rw || g.panoBmp->height() != rh) {
            g.panoBmp = Bitmap::Create(rw, rh);
            g.panoKey = App::PanoKey{};
        }
    } catch (...) { g.panoBmp.reset(); return; }
    App::PanoKey key{sources.front(), g.imageId, g.settings, g.pano.yaw, g.pano.pitch, g.pano.fov, rw, rh};
    if (!(key == g.panoKey)) {
        std::vector<PanoSource> levels;
        for (const Bitmap* b : sources) levels.push_back({b->bits(), b->width(), b->height()});
        RenderPano(levels.data(), static_cast<int>(levels.size()), g.pano, g.panoBmp->bits(), rw, rh);
        GdiFlush();
        g.panoKey = key;
    }
    HDC memDC = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(memDC, g.panoBmp->handle());
    if (div == 1) {
        BitBlt(dc, vp.left, vp.top, vw, vh, memDC, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, vp.left, vp.top, vw, vh, memDC, 0, 0, rw, rh, SRCCOPY);
    }
    SelectObject(memDC, old);
    DeleteDC(memDC);
}

void DrawImage(HDC dc, const RECT& vp) {
    if (PanoActive()) {
        std::vector<const Bitmap*> sources;
        const bool previewCurrent = g.previewBmp && g.previewSettings == g.settings;
        if (FullIsCurrent() || (!previewCurrent && !g.levels.empty())) {
            for (const auto& lv : g.levels) sources.push_back(lv.get());
        } else if (g.previewBmp) {
            sources.push_back(g.previewBmp.get());
        }
        DrawPano(dc, vp, sources);
        return;
    }
    HDC memDC = CreateCompatibleDC(dc);
    const Bitmap* src = nullptr;

    const bool previewCurrent = g.previewBmp && g.previewSettings == g.settings;
    if (FullIsCurrent() || (!previewCurrent && !g.levels.empty())) {
        // Smallest level that still has at least one pixel per screen pixel.
        const double needW = g.image->width * g.zoom - 0.5;
        src = g.levels.front().get();
        for (const auto& lv : g.levels)
            if (lv->width() >= needW) src = lv.get();
    } else if (g.previewBmp) {
        src = g.previewBmp.get();
    }
    if (src) DrawBitmapView(dc, memDC, *src, vp);
    DeleteDC(memDC);
}

// While a file opened from the grid (or by the arrow keys) loads, its
// thumbnail is shown enlarged so the switch feels immediate.
GridItem* PlaceholderItem() {
    if (!g.loadBusy || g.items.empty()) return nullptr;
    const std::wstring& target = TargetPath();
    if (SamePath(target, g.path)) return nullptr;
    int i = GridIndexOf(target);
    if (i < 0) return nullptr;
    GridItem& it = g.items[static_cast<size_t>(i)];
    return (it.state == GridItem::State::Done && !it.thumb.empty()) ? &it : nullptr;
}

void DrawPlaceholder(HDC dc, const RECT& vp, GridItem& it) {
    try {
        bool fresh = false;
        if (!it.bmp || it.bmp->width() != it.thumb.width || it.bmp->height() != it.thumb.height) {
            it.bmp = Bitmap::Create(it.thumb.width, it.thumb.height);
            fresh = true;
        }
        if (fresh || it.bmpSettings != g.settings) {
            TonemapThumbnail(it.thumb, g.settings, it.bmp->bits());
            GdiFlush();
            it.bmpSettings = g.settings;
        }
    } catch (...) { it.bmp.reset(); return; }

    if (g.panoOn && LooksPanoramic(it.imageWidth, it.imageHeight)) {   // keep looking the same way
        DrawPano(dc, vp, {it.bmp.get()});
        return;
    }
    const int aw = vp.right - vp.left, ah = vp.bottom - vp.top;
    const double s = std::min({static_cast<double>(aw) / it.imageWidth, static_cast<double>(ah) / it.imageHeight, 1.0});
    const int dw = std::max(1, static_cast<int>(it.imageWidth * s)), dh = std::max(1, static_cast<int>(it.imageHeight * s));
    HDC memDC = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(memDC, it.bmp->handle());
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, vp.left + (aw - dw) / 2, vp.top + (ah - dh) / 2, dw, dh,
               memDC, 0, 0, it.bmp->width(), it.bmp->height(), SRCCOPY);
    SelectObject(memDC, old);
    DeleteDC(memDC);
}

void FillRoundRect(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

int TextWidth(HDC dc, const std::wstring& t) {
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return r.right - r.left;
}

// Info bar: `left` text, then an optional clickable `chip` (the EXR layer),
// and `right` text aligned to the right edge.
void DrawInfoText(HDC dc, const RECT& bar, const std::wstring& left, const std::wstring& right,
                  const std::wstring& chip = std::wstring()) {
    HBRUSH b = CreateSolidBrush(kBarColor);
    FillRect(dc, &bar, b);
    DeleteObject(b);

    RECT tr = bar;
    tr.left += Scale(10);
    tr.right -= Scale(10);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.font);
    SetTextColor(dc, kDimText);

    const int rightW = TextWidth(dc, right);
    RECT rightRect = tr; rightRect.left = std::max(tr.left, tr.right - rightW);
    DrawTextW(dc, right.c_str(), -1, &rightRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const int chipW = chip.empty() ? 0 : TextWidth(dc, chip) + Scale(30);
    const int leftMax = std::max(0, static_cast<int>(rightRect.left - Scale(16) - tr.left) - (chipW ? chipW + Scale(10) : 0));
    const int leftW = std::min(TextWidth(dc, left), leftMax);
    RECT leftRect = tr; leftRect.right = tr.left + leftW;
    DrawTextW(dc, left.c_str(), -1, &leftRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    g.layerChip = RECT{};
    if (chipW) {
        const int h = Scale(20), cy = (bar.top + bar.bottom) / 2;
        RECT c{leftRect.right + Scale(10), cy - h / 2, leftRect.right + Scale(10) + chipW, cy - h / 2 + h};
        if (c.right > rightRect.left - Scale(8)) c.right = rightRect.left - Scale(8);
        if (c.right - c.left > Scale(40)) {
            g.layerChip = c;
            FillRoundRect(dc, c, g.chipHot ? RGB(0x46, 0x46, 0x46) : RGB(0x34, 0x34, 0x34), Scale(4));
            RECT t = c; t.left += Scale(8); t.right -= Scale(18);
            SetTextColor(dc, RGB(0xdd, 0xdd, 0xdd));
            DrawTextW(dc, chip.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            // small "opens a menu" triangle
            const int tx = c.right - Scale(12), ty = cy, ts = Scale(3);
            POINT pts[3] = {{tx - ts, ty - ts / 2}, {tx + ts + 1, ty - ts / 2}, {tx, ty + ts}};
            HBRUSH tb = CreateSolidBrush(RGB(0xbb, 0xbb, 0xbb));
            HGDIOBJ ob = SelectObject(dc, tb);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, pts, 3);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(tb);
        }
    }
}

std::wstring FormatValue(float v) {
    wchar_t buf[32];
    if (v != v) return L"NaN";
    if (std::isinf(v)) return v > 0 ? L"inf" : L"-inf";
    const float a = std::fabs(v);
    if (a >= 10000.0f) std::swprintf(buf, std::size(buf), L"%.0f", static_cast<double>(v));
    else if (a >= 100.0f) std::swprintf(buf, std::size(buf), L"%.2f", static_cast<double>(v));
    else std::swprintf(buf, std::size(buf), L"%.4f", static_cast<double>(v));
    return buf;
}

std::wstring PixelText() {
    wchar_t buf[64];
    std::swprintf(buf, std::size(buf), L"x %d  y %d", g.pixelX, g.pixelY);
    if (const int set = CurrentCrypto(); set >= 0) {
        // Cryptomatte: the objects under the mouse instead of colour values.
        std::wstring t = std::wstring(buf) + L"     ";
        if (g.pixelHits.empty()) return t + L"(no object)";
        for (size_t k = 0; k < std::min<size_t>(2, g.pixelHits.size()); ++k) {
            if (k) t += L"  \u00b7  ";
            std::swprintf(buf, std::size(buf), L" %.0f%%", static_cast<double>(g.pixelHits[k].coverage) * 100.0);
            t += Utf8ToWide(g.doc->CryptoName(set, g.pixelHits[k].id)) + buf;
        }
        return t;
    }
    return std::wstring(buf) + L"     R " + FormatValue(g.pixel[0]) + L"   G " + FormatValue(g.pixel[1]) +
           L"   B " + FormatValue(g.pixel[2]);
}

const wchar_t* kSep = L"   \u2022   ";

std::wstring BatchProgressText() {
    wchar_t buf[96];
    std::swprintf(buf, std::size(buf), L"Exporting %d of %d\u2026  (Esc to cancel)",
                  std::min(g.batchDone + 1, g.batchTotal), g.batchTotal);
    return buf;
}

// Slider geometry inside g.sizeSlider: the track and the value under it.
void SizeTrack(int& x0, int& x1) {
    x0 = g.sizeSlider.left + Scale(40);
    x1 = g.sizeSlider.right - Scale(48);
}

float SizeFromX(int x) {
    int x0, x1;
    SizeTrack(x0, x1);
    const float t = std::clamp(static_cast<float>(x - x0) / std::max(1, x1 - x0), 0.0f, 1.0f);
    float s = 0.5f + 1.5f * t;
    if (std::fabs(s - 1.0f) < 0.06f) s = 1.0f;   // easy to land on 100%
    return s;
}

void DrawSizeSlider(HDC dc, const RECT& bar) {
    g.sizeSlider = RECT{bar.right - Scale(190), bar.top, bar.right - Scale(8), bar.bottom};
    int x0, x1;
    SizeTrack(x0, x1);
    const int cy = (bar.top + bar.bottom) / 2;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.font);
    SetTextColor(dc, kDimText);
    RECT label = {g.sizeSlider.left, bar.top, x0 - Scale(8), bar.bottom};
    DrawTextW(dc, L"Size", -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const int th = std::max(2, Scale(3));
    RECT track = {x0, cy - th / 2, x1, cy - th / 2 + th};
    FillRoundRect(dc, track, RGB(0x4a, 0x4a, 0x4a), th / 2);
    const int tx = x0 + static_cast<int>((g.thumbScale - 0.5f) / 1.5f * (x1 - x0) + 0.5f);
    const int mark = x0 + static_cast<int>((1.0f - 0.5f) / 1.5f * (x1 - x0) + 0.5f);   // 100%
    RECT tick = {mark - std::max(1, Scale(1)) / 2, cy - Scale(5), mark + std::max(1, Scale(1)) / 2 + 1, cy + Scale(5)};
    HBRUSH tb = CreateSolidBrush(RGB(0x70, 0x70, 0x70));
    FillRect(dc, &tick, tb);
    DeleteObject(tb);
    const int r = Scale(6);
    RECT knob = {tx - r, cy - r, tx + r, cy + r};
    FillRoundRect(dc, knob, (g.sizeHot || g.drag == Drag::SizeSlider) ? RGB(0xff, 0xff, 0xff) : RGB(0xd0, 0xd0, 0xd0), r);

    wchar_t buf[16];
    std::swprintf(buf, std::size(buf), L"%d%%", static_cast<int>(g.thumbScale * 100.0f + 0.5f));
    RECT val = {x1 + Scale(10), bar.top, g.sizeSlider.right, bar.bottom};
    DrawTextW(dc, buf, -1, &val, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawInfoBar(HDC dc, const RECT& bar) {
    std::wstring left, right;
    wchar_t buf[128];

    if (g.mode == Mode::Grid) {
        const int n = static_cast<int>(g.items.size());
        const int shown = g.gridHover >= 0 ? g.gridHover : g.selected;
        if (shown >= 0 && shown < n) {
            const GridItem& it = g.items[static_cast<size_t>(shown)];
            left = it.name;
            if (it.state == GridItem::State::Done) {
                std::swprintf(buf, std::size(buf), L"%d \u00d7 %d", it.imageWidth, it.imageHeight);
                left += kSep; left += buf;
                left += kSep; left += Utf8ToWide(it.format);
            } else if (it.state == GridItem::State::Failed) {
                left += kSep; left += Utf8ToWide(it.error);
            }
        } else {
            left = g.gridFolder;
        }
        const int remaining = g.pool ? g.pool->Remaining() : 0;
        const int picked = PickedCount();
        if (g.batchBusy) {
            right = BatchProgressText();
        } else if (!g.status.empty()) {
            right = g.status;
        } else if (g.loadBusy) {
            right = L"Paused while an image opens";
        } else if (remaining > 0) {
            std::swprintf(buf, std::size(buf), L"Making thumbnails\u2026 %d left", remaining);
            right = buf;
        } else {
            std::swprintf(buf, std::size(buf), n == 1 ? L"%d image" : L"%d images", n);
            right = buf;
        }
        if (picked > 1) {
            std::swprintf(buf, std::size(buf), L"%d selected", picked);
            right = std::wstring(buf) + kSep + right;
        }
        // Thumbnail size slider at the right end.
        RECT textBar = bar;
        textBar.right -= Scale(190);
        DrawInfoText(dc, textBar, left, right);
        DrawSizeSlider(dc, bar);
        return;
    }
    g.sizeSlider = RECT{};

    std::wstring chip;
    if (HasImage()) {
        left = FileNameOf(g.path);
        NavInfo n = CurrentNav();
        if (n.exact && n.count > 1 && !g.loadBusy) {
            std::swprintf(buf, std::size(buf), L"%d of %d", n.index + 1, n.count);
            left += kSep; left += buf;
        }
        std::swprintf(buf, std::size(buf), L"%d \u00d7 %d", g.image->width, g.image->height);
        left += kSep; left += buf;
        left += kSep; left += Utf8ToWide(g.image->format);

        const double pct = g.zoom * 100.0;
        if (PanoActive())
            std::swprintf(buf, std::size(buf), L"360\u00b0   yaw %+.0f\u00b0   pitch %+.0f\u00b0   FOV %.0f\u00b0",
                          g.pano.yaw, g.pano.pitch, g.pano.fov);
        else if (pct < 10.0) std::swprintf(buf, std::size(buf), L"%.1f%%", pct);
        else std::swprintf(buf, std::size(buf), L"%.0f%%", pct);
        right = buf;
        if (g.pixelValid) right = PixelText() + kSep + right;

        if (g.doc && !g.loadBusy) {
            const int li = CurrentLayer();
            if (li >= 0 && li < LayerCount()) {
                const ExrLayer& L = g.doc->Layers()[static_cast<size_t>(li)];
                chip = L"Layer: " + Utf8ToWide(ExrDocument::DisplayName(L)) + L"  \u00b7  " + Utf8ToWide(L.channels);
                if (L.view != LayerView::Color) {
                    chip += L"  \u00b7  " + Utf8ToWide(ExrDocument::ViewName(L.view));
                    if (L.view == LayerView::Depth && g.viewOpts.invertDepth) chip += L" (inverted)";
                }
            }
        }
    } else {
        left = L"No image open";
    }

    if (const std::wstring iso = IsolationText(); !iso.empty() && !g.loadBusy)
        right = iso + L"  (Esc shows all)" + kSep + right;
    if (g.batchBusy) right = BatchProgressText() + kSep + right;

    if (FixActive() && !g.loadBusy && !g.layerBusy) {
        left += kSep;
        left += g.showingOriginal ? L"Original (before fix)" : g.fixSaved ? L"Fixed, saved" : L"Fixed, not saved";
    }
    if (g.loadBusy) {
        right = L"Loading " + FileNameOf(TargetPath()) + L"\u2026";
    } else if (g.fixBusy) {
        right = g.fixBusyText + std::wstring(kSep) + right;
    } else if (g.exportBusy) {
        right = (g.exportLookUsed == -2 ? L"Saving\u2026" : L"Exporting\u2026") + std::wstring(kSep) + right;
    } else if (!g.status.empty()) {
        right = right.empty() ? g.status : g.status + kSep + right;
    } else if (g.layerBusy) {
        right = L"Switching layer\u2026" + std::wstring(kSep) + right;
    } else if (HasImage() && !FullIsCurrent()) {
        right = L"Updating full resolution\u2026" + std::wstring(kSep) + right;
    }
    DrawInfoText(dc, bar, left, right, chip);
}

// --- side panels ----------------------------------------------------------------------------

PanelFonts MakePanelFonts() { return PanelFonts{g.font, g.boldFont, g.smallFont, g.titleFont}; }

SidebarState MakeSidebarState() {
    SidebarState st;
    st.rows = &g.rows;
    st.current = CurrentRow();
    st.searching = g.searchActive;
    st.matches = g.rowMatches;
    st.scopeAll = g.searchAll;
    st.scopeThisEnabled = !(g.searchActive ? g.searchFolder : (g.mode == Mode::Grid ? g.gridFolder : FolderOf(TargetPath()))).empty();
    st.status = LibraryStatus();
    st.hover = g.sideHover;
    return st;
}

// "104 MB", "850 KB".
std::wstring FormatBytes(uint64_t b) {
    wchar_t buf[32];
    if (b >= 1000ull * 1000 * 1000) std::swprintf(buf, std::size(buf), L"%.1f GB", b / 1e9);
    else if (b >= 1000ull * 1000) std::swprintf(buf, std::size(buf), L"%.0f MB", b / 1e6);
    else std::swprintf(buf, std::size(buf), L"%.0f KB", std::max(1.0, b / 1e3));
    return buf;
}

// A file time as a local date: "29 Oct 2026, 14:13".
std::wstring FormatFileTime(uint64_t t) {
    if (!t) return {};
    FILETIME ft{static_cast<DWORD>(t & 0xffffffffu), static_cast<DWORD>(t >> 32)}, local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t buf[64];
    std::swprintf(buf, std::size(buf), L"%u %ls %u, %02u:%02u", st.wDay, months[(st.wMonth + 11) % 12], st.wYear, st.wHour, st.wMinute);
    return buf;
}

// 85479.2 -> "85,479"; small values keep decimals.
std::wstring BigNumber(float v) {
    wchar_t buf[48];
    if (v < 10.0f) { std::swprintf(buf, std::size(buf), L"%.2f", static_cast<double>(v)); return buf; }
    if (v < 1000.0f) { std::swprintf(buf, std::size(buf), L"%.1f", static_cast<double>(v)); return buf; }
    std::swprintf(buf, std::size(buf), L"%.0f", static_cast<double>(v));
    std::wstring t = buf, out;
    for (size_t i = 0; i < t.size(); ++i) {
        out += t[i];
        const size_t left = t.size() - 1 - i;
        if (left > 0 && left % 3 == 0) out += L',';
    }
    return out;
}

void AddBrightnessStats(InspectorInfo& info, float peak, float range, bool approx) {
    if (peak < 0.0f) return;
    InspectorStat a{L"Brightest", (approx ? L"≈ " : L"") + BigNumber(peak), peak <= 1.0f};
    wchar_t buf[48];
    std::swprintf(buf, std::size(buf), L"%.1f stops", static_cast<double>(range));
    InspectorStat b{L"Range", buf, false};
    info.stats = {a, b};
    if (peak <= 1.0f) info.note = L"Nothing is brighter than 1.0: an ordinary picture saved as HDR.";
    else if (approx) info.note = L"Measured on a reduced copy of this very large file.";
}

// The inspector's small preview: the selected tile's thumbnail, tonemapped like the grid.
const Bitmap* InspectorPreview(const GridItem& it) {
    if (it.thumb.empty()) return nullptr;
    if (!g.inspBmp || g.inspBmpPath != it.path || g.inspBmpSettings != g.settings ||
        g.inspBmp->width() != it.thumb.width || g.inspBmp->height() != it.thumb.height) {
        try {
            g.inspBmp = Bitmap::Create(it.thumb.width, it.thumb.height);
            TonemapThumbnail(it.thumb, g.settings, g.inspBmp->bits());
            g.inspBmpPath = it.path;
            g.inspBmpSettings = g.settings;
        } catch (...) {
            g.inspBmp.reset();
            g.inspBmpPath.clear();
            return nullptr;
        }
    }
    return g.inspBmp.get();
}

// Brightest value and range of the image on screen, measured once per image.
void EnsureImageStats() {
    if (!HasImage() || g.statsFor == g.image.get()) return;
    ImageQuickStats(*g.image, g.imagePeak, g.imageRange);
    g.statsFor = g.image.get();
}

int LayerCount();
int CurrentLayer();

InspectorInfo MakeInspectorInfo() {
    InspectorInfo info;
    wchar_t buf[160];
    if (g.mode == Mode::Grid) {
        info.emptyText = g.items.empty()
            ? (g.searchActive ? L"No file names match your search."
               : g.library.Size() == 0 ? L"Add the folders where you keep HDRIs and EXRs with “+ Add” on the left. "
                                          L"You can also open a folder with Ctrl+Shift+O or drop one here."
                                        : L"No .hdr or .exr files in this folder.")
            : L"Select an image to see its details.";
        const int picked = PickedCount();
        if (picked > 1) {
            uint64_t total = 0;
            for (const auto& it : g.items) if (it.picked) total += it.fileSize;
            info.title = std::to_wstring(picked) + L" images selected";
            info.lines.push_back(FormatBytes(total) + L" in total");
            info.buttons.push_back({L"Export " + std::to_wstring(picked) + L" images…", IDM_EXPORT, true, !g.batchBusy});
            info.buttons.push_back({L"Select all", IDM_SELECT_ALL, false, true});
            return info;
        }
        if (g.selected < 0 || g.selected >= static_cast<int>(g.items.size())) return info;
        const GridItem& it = g.items[static_cast<size_t>(g.selected)];
        info.title = it.name;
        info.preview = InspectorPreview(it);
        if (it.state == GridItem::State::Done) {
            std::swprintf(buf, std::size(buf), L"%d × %d · %ls · %ls", it.imageWidth, it.imageHeight,
                          Utf8ToWide(it.format).c_str(), FormatBytes(it.fileSize).c_str());
            info.lines.push_back(buf);
        } else if (it.state == GridItem::State::Failed) {
            info.lines.push_back(Utf8ToWide(it.error));
        } else {
            info.lines.push_back(FormatBytes(it.fileSize) + L" · reading…");
        }
        if (const std::wstring d = FormatFileTime(it.writeTime); !d.empty()) info.lines.push_back(L"Modified " + d);
        info.path = FolderOf(it.path);
        if (it.state == GridItem::State::Done) AddBrightnessStats(info, it.peakLum, it.rangeStops, it.statsReduced);
        info.buttons.push_back({L"Open", IDM_IMAGE, true, it.state != GridItem::State::Failed});
        info.buttons.push_back({L"Show in Explorer", IDM_SHOW_IN_EXPLORER, false, true});
        return info;
    }
    if (!HasImage()) {
        info.emptyText = g.loadBusy ? L"Opening…" : L"Open an image or a folder to see its details here.";
        return info;
    }
    EnsureImageStats();
    info.title = FileNameOf(g.path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    std::wstring size;
    if (GetFileAttributesExW(g.path.c_str(), GetFileExInfoStandard, &fa))
        size = L" · " + FormatBytes((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
    std::swprintf(buf, std::size(buf), L"%d × %d · %ls", g.image->width, g.image->height, Utf8ToWide(g.image->format).c_str());
    info.lines.push_back(buf + size);
    if (g.doc && LayerCount() > 1) {
        const ExrLayer& L = g.doc->Layers()[static_cast<size_t>(std::clamp(CurrentLayer(), 0, LayerCount() - 1))];
        info.lines.push_back(L"Layer: " + Utf8ToWide(ExrDocument::DisplayName(L)) + L" (" + std::to_wstring(LayerCount()) + L" in this file)");
    }
    info.path = FolderOf(g.path);
    if (!ViewIsData()) AddBrightnessStats(info, g.imagePeak, g.imageRange, false);
    if (FixActive())
        info.note = g.showingOriginal ? L"Showing the original. B shows the fixed copy."
                                      : g.fixSaved ? L"Showing the fixed copy (saved)." : L"Showing the fixed copy, not saved yet: Ctrl+S saves it.";
    info.buttons.push_back({L"Fix HDRI…", IDM_FIX, true, (CanFix() || FixActive()) && !g.fixBusy});
    info.buttons.push_back({L"Sun check", IDM_SUN_CHECK, false, !ViewIsData()});
    if (LooksPanoramic(g.image->width, g.image->height))
        info.buttons.push_back({PanoActive() ? L"Flat view" : L"360° view", IDM_PANO, false, true});
    info.buttons.push_back({L"Show in Explorer", IDM_SHOW_IN_EXPLORER, false, true});
    return info;
}

// The strip above the grid: what is showing, and "Include subfolders" for library folders.
RECT g_subToggle{};

void DrawGridHeader(HDC dc, const RECT& r) {
    PanelFill(dc, r, kBackground);
    RECT line{r.left, r.bottom - 1, r.right, r.bottom};
    PanelFill(dc, line, panel::kLine);
    const int pad = Scale(18);
    g_subToggle = RECT{};
    std::wstring title, right;
    wchar_t buf[200];
    const int n = static_cast<int>(g.items.size());
    if (g.searchActive) {
        if (g.searchHits < 5000) std::swprintf(buf, std::size(buf), g.searchHits == 1 ? L"%zu result for “%ls”"
                                                                                       : L"%zu results for “%ls”",
                                               g.searchHits, g.searchQuery.c_str());
        else std::swprintf(buf, std::size(buf), L"5,000+ results for “%ls”", g.searchQuery.c_str());
        title = buf;
        right = (!g.searchAll && !g.searchFolder.empty()) ? L"in " + LeafName(g.searchFolder) : L"in all folders";
        right += L"   ·   Esc clears";
    } else {
        title = g.gridFolder;
        std::swprintf(buf, std::size(buf), n == 1 ? L"%d image" : L"%d images", n);
        right = buf;
    }
    RECT rr{r.left + pad, r.top, r.right - pad, r.bottom};
    // "Include subfolders" for folders in the library.
    const int root = g.searchActive ? -1 : g.library.RootOf(g.gridFolder);
    int rightEdge = rr.right;
    if (root >= 0) {
        const bool on = g.library.Folders()[static_cast<size_t>(root)].subfolders;
        const int boxS = Scale(14), w = Scale(140);
        g_subToggle = RECT{rr.right - w, r.top + Scale(6), rr.right, r.bottom - Scale(6)};
        const int bx = g_subToggle.left, by = (r.top + r.bottom) / 2 - boxS / 2;
        RECT box{bx, by, bx + boxS, by + boxS};
        PanelRound(dc, box, on ? panel::kAccent : panel::kButtonHot, Scale(3));
        if (on) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, Scale(2)), RGB(0xff, 0xff, 0xff));
            HGDIOBJ op = SelectObject(dc, pen);
            POINT pts[3] = {{bx + boxS * 22 / 100, by + boxS * 52 / 100}, {bx + boxS * 42 / 100, by + boxS * 72 / 100},
                            {bx + boxS * 78 / 100, by + boxS * 30 / 100}};
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        PanelText(dc, g.font, panel::kSoft, L"Include subfolders", RECT{bx + boxS + Scale(8), r.top, g_subToggle.right, r.bottom},
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rightEdge = g_subToggle.left - Scale(20);
    }
    const int rw = Scale(static_cast<int>(right.size()) * 7 + 10);
    RECT countR{std::max<LONG>(rr.left, rightEdge - rw), r.top, rightEdge, r.bottom};
    PanelText(dc, g.font, panel::kDim, right, countR, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT titleR{rr.left, r.top, std::max<LONG>(rr.left, countR.left - Scale(12)), r.bottom};
    PanelText(dc, g.titleFont, panel::kText, title, titleR,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | (g.searchActive ? DT_END_ELLIPSIS : DT_PATH_ELLIPSIS));
}

bool SelectedOpenable() {
    return g.selected >= 0 && g.selected < static_cast<int>(g.items.size()) &&
           g.items[static_cast<size_t>(g.selected)].state != GridItem::State::Failed;
}

ToolbarState MakeToolbarState() {
    ToolbarState st;
    const bool grid = g.mode == Mode::Grid;
    st.exposure = g.settings.exposureEV;
    st.curve = g.settings.curve;
    st.gridMode = grid;
    st.canGrid = grid || !TargetPath().empty() || !g.gridFolder.empty();
    st.canImage = grid ? SelectedOpenable() : HasImage();
    st.hasImage = grid ? !g.items.empty() : HasImage();
    st.viewControls = !grid && HasImage();
    st.fit = g.fit;
    st.actualSize = std::fabs(g.zoom - 1.0) < 1e-9;
    st.canPano = !grid && HasImage() && LooksPanoramic(g.image->width, g.image->height);
    st.pano = PanoActive();
    st.canFix = CanFix() || FixActive();
    st.fixOn = FixActive() && !g.showingOriginal;
    if (grid) {
        st.canPrev = g.selected > 0;
        st.canNext = g.selected >= 0 && g.selected + 1 < static_cast<int>(g.items.size());
    } else {
        st.canPrev = CanGoPrev();
        st.canNext = CanGoNext();
    }
    st.hover = g.hover;
    st.pressed = g.pressed;
    return st;
}

void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT client = ClientRect();
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0) { EndPaint(hwnd, &ps); return; }

    // Draw off-screen first to avoid flicker.
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cw, ch);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g.font);

    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    const RECT vp = Viewport();
    RECT bar = client; bar.top = vp.bottom;

    if (g.mode == Mode::Grid) {
        LayoutGrid();
        if (g.items.empty()) {
            if (g.searchActive)
                DrawCenteredText(dc, vp, L"No file names match \u201c" + g.searchQuery + L"\u201d",
                                 g.searchAll ? L"Searched all library folders" : L"Searched " + g.searchFolder);
            else if (g.gridFolder.empty())
                DrawCenteredText(dc, vp, L"Add a folder with \u201c+ Add\u201d on the left, or drop one here",
                                 L"Ctrl+Shift+O opens a folder, Ctrl+O a file");
            else
                DrawCenteredText(dc, vp, L"No .hdr or .exr files in this folder", g.gridFolder);
        } else {
            int first, last;
            g.grid.VisibleRange(first, last);
            if (g.pool) g.pool->SetFocus(first, last);
            g.grid.Draw(dc, vp, g.items, g.settings, g.selected, g.gridHover,
                        g.scrollHot || g.drag == Drag::Scroll, GridFonts{g.font, g.font});
        }
        DrawGridHeader(dc, GridHeaderRect());   // after the tiles: a half-scrolled row stays under it
    } else if (GridItem* ph = PlaceholderItem()) {
        DrawPlaceholder(dc, vp, *ph);
    } else if (HasImage()) {
        DrawImage(dc, vp);
    } else if (g.loadBusy) {
        DrawCenteredText(dc, vp, L"Loading " + FileNameOf(TargetPath()) + L"\u2026", L"");
    } else {
        DrawCenteredText(dc, vp, L"Drop an .hdr or .exr file, or a folder, here",
                         L"Ctrl+O opens a file, Ctrl+Shift+O a folder of images");
    }
    if (SidebarShown()) g.sidebar.Draw(dc, MakeSidebarState(), MakePanelFonts());
    if (InspectorShown() && !g.fixPanelOn) {
        g.inspector.Layout(InspectorRect(), g.dpi);
        g.inspector.Draw(dc, MakeInspectorInfo(), MakePanelFonts(), g.inspHover, g.inspPressed);
    }
    DrawInfoBar(dc, bar);
    g.toolbar.Draw(dc, MakeToolbarState(), g.font);

    BitBlt(wdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
           ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// --- commands -----------------------------------------------------------------------------

// Help > Controls and shortcuts (F1): keys and mouse actions in aligned columns.
void ShowShortcuts() {
    InfoPage page;
    page.windowTitle = L"Controls and shortcuts";
    page.title = L"Controls and shortcuts";
    page.subtitle = L"EXRay \u2014 keyboard and mouse";
    page.columnWidth = 330;
    page.columns = {
        {
            InfoHeader(L"Files and folders"),
            InfoKeys(L"Ctrl|O", L"Open a file"),
            InfoKeys(L"Ctrl|Shift|O", L"Open a folder as a grid"),
            InfoKeys(L"Drop", L"Drop a file or folder on the window"),
            InfoKeys(L"F5", L"Refresh the folder"),
            InfoKeys(L"\u2190|\u2192", L"Previous / next file"),
            InfoKeys(L"Home|End", L"First / last file"),
            InfoKeys(L"Ctrl|E", L"Export as PNG or JPEG"),

            InfoHeader(L"Folders panel and search"),
            InfoKeys(L"Ctrl|F", L"Search file names in your folders"),
            InfoKeys(L"Esc", L"In the search box: clear the search"),
            InfoKeys(L"Ctrl|B", L"Show / hide the folders panel"),
            InfoKeys(L"Ctrl|I", L"Show / hide the inspector"),
            InfoKeys(L"Right-click", L"On a folder: rescan, subfolders, remove"),
            InfoKeys(L"Ctrl|Z", L"Bring back a removed folder"),

            InfoHeader(L"Switching views"),
            InfoKeys(L"G", L"Show the folder grid"),
            InfoKeys(L"Esc", L"From an image back to the grid"),
            InfoKeys(L"Enter", L"Open the selected image"),
            InfoKeys(L"Double-click", L"Open the image under the mouse"),
            InfoKeys(L"F1", L"This window"),

            InfoHeader(L"360\u00b0 view"),
            InfoKeys(L"V", L"Turn the 360\u00b0 view on / off"),
            InfoKeys(L"Drag", L"Look around"),
            InfoKeys(L"Wheel", L"Field of view (zoom)"),
            InfoKeys(L"Double-click", L"Reset the view"),
        },
        {
            InfoHeader(L"Grid"),
            InfoKeys(L"Arrows", L"Move the selection"),
            InfoKeys(L"Shift|Arrows", L"Extend the selection"),
            InfoKeys(L"Ctrl|Click", L"Add or remove one image"),
            InfoKeys(L"Shift|Click", L"Select a range"),
            InfoKeys(L"Ctrl|A", L"Select all"),
            InfoKeys(L"Right-click", L"Open / export menu"),
            InfoKeys(L"Ctrl|Wheel", L"Thumbnail size"),
            InfoKeys(L"Ctrl|0", L"Thumbnails back to 100%"),

            InfoHeader(L"Exposure and tonemap"),
            InfoKeys(L"+|\u2212", L"Exposure up / down by 0.5 EV"),
            InfoKeys(L"R", L"Reset exposure to 0"),
            InfoKeys(L"T", L"Next tonemap: Clip, Reinhard, ACES"),
            InfoKeys(L"Wheel", L"On the slider: fine steps"),
            InfoKeys(L"Double-click", L"On the slider: reset"),

            InfoHeader(L"Fix HDRI"),
            InfoKeys(L"H", L"Fix HDRI panel: check the HDRI, choose fixes (H again closes it)"),
            InfoKeys(L"B", L"Before / after the fix"),
            InfoKeys(L"Ctrl|S", L"Save the fixed copy (EXR or HDR)"),
            InfoKeys(L"S", L"Sun check: look at the sun, exposed for a real sun"),
        },
        {
            InfoHeader(L"Image view"),
            InfoKeys(L"Wheel", L"Zoom in / out at the mouse"),
            InfoKeys(L"Drag", L"Move around a zoomed image"),
            InfoKeys(L"Double-click", L"Switch between Fit and 100%"),
            InfoKeys(L"F", L"Fit to window"),
            InfoKeys(L"1", L"100%: one image pixel per screen pixel"),
            InfoKeys(L"Ctrl|+", L"Zoom in"),
            InfoKeys(L"Ctrl|\u2212", L"Zoom out"),
            InfoKeys(L"Ctrl|C", L"Copy the pixel values"),

            InfoHeader(L"EXR layers"),
            InfoKeys(L"L", L"Choose a layer"),
            InfoKeys(L"[|]", L"Previous / next layer"),
            InfoKeys(L"I", L"Invert a depth view"),

            InfoHeader(L"Cryptomatte"),
            InfoKeys(L"Click", L"Show only that object as a mask"),
            InfoKeys(L"Ctrl|Click", L"Add or remove objects"),
            InfoKeys(L"Esc", L"Show all objects again"),
        },
    };
    ShowInfoWindow(g.hwnd, page);
}

// The notices the included libraries ask to be shown with the program.
void ShowLicenses(HWND owner) {
    const wchar_t* mit =
        L"Permission is hereby granted, free of charge, to any person obtaining a copy of this software and "
        L"associated documentation files (the \"Software\"), to deal in the Software without restriction, including "
        L"without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
        L"copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
        L"following conditions: The above copyright notice and this permission notice shall be included in all "
        L"copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF "
        L"ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR "
        L"A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE "
        L"FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING "
        L"FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.";
    InfoPage page;
    page.windowTitle = L"Licenses";
    page.title = L"Licenses";
    page.subtitle = L"EXRay's own license, and the libraries it includes";
    page.columnWidth = 360;
    page.columns = {
        {
            InfoHeader(L"EXRay \u2014 GNU GPL v3 or later"),
            InfoText(L"Copyright (c) 2026 Giorgos Eleftheroglou (pixelrise)."),
            InfoText(L"This program is free software: you can redistribute it and/or modify it under the terms of "
                     L"the GNU General Public License as published by the Free Software Foundation, either version 3 "
                     L"of the License, or (at your option) any later version. It is distributed in the hope that it "
                     L"will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY "
                     L"or FITNESS FOR A PARTICULAR PURPOSE. The full license is in LICENSE.txt next to the program."),
            InfoText(L"The name \u201cEXRay\u201d and its logo are not covered by this license: modified versions "
                     L"must use another name and logo (see TRADEMARKS.md with the source code)."),
            InfoHeader(L"tinyexr \u2014 BSD 3-Clause License"),
            InfoText(L"Copyright (c) 2014 - 2021, Syoyo Fujita and many contributors. All rights reserved."),
            InfoText(L"Redistribution and use in source and binary forms, with or without modification, are "
                     L"permitted provided that the following conditions are met: 1. Redistributions of source "
                     L"code must retain the above copyright notice, this list of conditions and the following "
                     L"disclaimer. 2. Redistributions in binary form must reproduce the above copyright notice, "
                     L"this list of conditions and the following disclaimer in the documentation and/or other "
                     L"materials provided with the distribution. 3. Neither the name of the copyright holder nor "
                     L"the names of its contributors may be used to endorse or promote products derived from this "
                     L"software without specific prior written permission."),
            InfoText(L"THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND ANY "
                     L"EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF "
                     L"MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE "
                     L"COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, "
                     L"EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF "
                     L"SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) "
                     L"HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR "
                     L"TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS "
                     L"SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."),
        },
        {
            InfoHeader(L"miniz \u2014 MIT License"),
            InfoText(L"Copyright 2013-2014 RAD Game Tools and Valve Software. "
                     L"Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC. All Rights Reserved."),
            InfoText(mit),
        },
        {
            InfoHeader(L"stb_image, stb_image_write \u2014 MIT License"),
            InfoText(L"Copyright (c) 2017 Sean Barrett. (Also available as public domain.)"),
            InfoText(mit),
            InfoHeader(L"Compiler runtime"),
            InfoText(L"Built with LLVM-MinGW. The LLVM C++ runtime is under the Apache License 2.0 with LLVM "
                     L"Exceptions; the mingw-w64 runtime is under permissive licenses. Neither requires notices "
                     L"in programs built with them; they are mentioned here for completeness."),
        },
    };
    ShowInfoWindow(owner, page);
}

// Help > About: what the tool does, what comes next, how it's built, contact.
void ShowAbout() {
    InfoPage page;
    page.windowTitle = L"About EXRay";
    page.title = L"EXRay";
    page.subtitle = std::wstring(L"Version ") + kVersion + L"  \u00b7  HDR & EXR viewer for 3D artists  \u00b7  Windows 11";
    page.columnWidth = 330;
    page.columns = {
        {
            InfoHeader(L"What it does"),
            InfoText(L"EXRay is a fast viewer for HDR images: Radiance .hdr files and OpenEXR .exr files, "
                     L"including multi-layer renders. It is a single .exe with nothing to install \u2014 "
                     L"copy it to any Windows 11 PC and run it."),
            InfoText(L"Browse a whole folder of HDRIs as thumbnails, open any image to inspect it with "
                     L"exposure and tonemap controls, look inside multi-pass renders, and export what "
                     L"you need as PNG or JPEG."),

            InfoHeader(L"Main features"),
            InfoBullet(L"Your HDRI and EXR folders in one place: add folders from any drive, browse them "
                       L"as a tree, and search file names across all of them."),
            InfoBullet(L"Thumbnail grid of a folder, made in the background, adjustable size."),
            InfoBullet(L"Inspector: size, brightest value and range of the selected image."),
            InfoBullet(L"Image view with zoom, pan, Fit and 100%, and arrow-key browsing."),
            InfoBullet(L"360\u00b0 view: look around inside an equirectangular HDRI."),
            InfoBullet(L"Exposure from \u221210 to +10 EV and Clip, Reinhard or ACES tonemapping."),
            InfoBullet(L"EXR layers, multi-part and multi-view files, with readable depth, "
                       L"normal and motion-vector views."),
            InfoBullet(L"Cryptomatte: ID colours, object names under the mouse, click to isolate a mask."),
            InfoBullet(L"Pixel readout of the real stored values."),
            InfoBullet(L"Export one image or a whole selection in the Clip, ACES or on-screen look, "
                       L"optionally every layer."),
            InfoBullet(L"Fix HDRI: rebuild a clipped sun, repair broken and hot pixels, and save the "
                       L"fixed copy as EXR or HDR, ready for 3D software."),
            InfoBullet(L"Handles 16K+ HDRIs; remembers its settings in EXRay.ini next to the .exe."),
        },
        {
            InfoHeader(L"Coming next"),
            InfoSubheader(L"v0.11 \u2014 Smart library"),
            InfoBullet(L"Badges on the thumbnails (clipped sun, real sun, low sun) and filters for them."),
            InfoBullet(L"Favourites, tags, recently opened; sort by brightness or sun height."),
            InfoSubheader(L"v0.12 \u2014 HDRI prep"),
            InfoBullet(L"Sun to light: its direction, strength and colour, ready to copy into Unreal or Blender."),
            InfoBullet(L"Calibrate: see how much light the HDRI gives (lux), scale it to a value you measured or a preset "
                       L"(sunny day, overcast, sunset), white balance from a grey spot, and convert colour spaces (sRGB / ACEScg)."),
            InfoBullet(L"Remove the sun; rotate and level the horizon; batch resize and convert."),
            InfoSubheader(L"v0.13 \u2014 Inspect"),
            InfoBullet(L"Histogram and false colour; chrome and grey ball lighting preview."),
            InfoBullet(L"Metadata panel: camera, lens and render settings stored in the file."),
            InfoSubheader(L"v1.0 \u2014 Launch"),
            InfoBullet(L"A/B compare of two files or layers; final testing on a clean Windows 11 PC."),
            InfoSubheader(L"Later"),
            InfoBullet(L"Thumbnails in File Explorer (optional installer), ARM64, float TIFF / PFM, "
                       L"DWAA / DWAB EXRs, exporting the 360\u00b0 view, 16-bit PNG."),
        },
        {
            InfoHeader(L"Free and open source"),
            InfoText(L"EXRay is free, with every feature and no time limit \u2014 for personal use and for "
                     L"paid client work. Its source code is open under the GNU GPL v3."),
            InfoText(L"If it saves you time, you can support it with a payment of your choice. It's optional "
                     L"and changes nothing in the app."),
            InfoAction(L"Source code on GitHub\u2026", [](HWND owner) {
                ShellExecuteW(owner, L"open", kSourcePage, nullptr, nullptr, SW_SHOWNORMAL);
            }),
            InfoAction(L"Sponsor on GitHub\u2026", [](HWND owner) {
                ShellExecuteW(owner, L"open", kSponsorPage, nullptr, nullptr, SW_SHOWNORMAL);
            }),
            InfoAction(L"Download page and support\u2026", [](HWND owner) {
                ShellExecuteW(owner, L"open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL);
            }),

            InfoHeader(L"Built with"),
            InfoBullet(L"C++17 and the Windows API (Win32, GDI and the standard file dialogs) \u2014 "
                       L"no frameworks, everything in one statically linked .exe."),
            InfoBullet(L"Compiled with Clang (LLVM-MinGW)."),
            InfoBullet(L"tinyexr and miniz for reading OpenEXR files."),
            InfoBullet(L"stb_image and stb_image_write for Radiance HDR, PNG and JPEG."),
            InfoBullet(L"Python with Pillow and NumPy for the app icon and the test tools."),
            InfoAction(L"Licenses of the included libraries\u2026", [](HWND owner) { ShowLicenses(owner); }),

            InfoHeader(L"Contact"),
            InfoSignature(L"Greece", L"pixelrise@gmail.com"),
            InfoText(L"Free. No ads, no tracking, works fully offline."),
        },
    };
    ShowInfoWindow(g.hwnd, page);
}

void GridMove(int delta) {
    if (g.items.empty()) return;
    SelectItem((g.selected < 0 ? 0 : g.selected) + delta, ShiftDown());
}

// Esc: stop an export, then show all Cryptomatte objects, then reduce a
// multi-selection to one tile, then go from an image to the grid.
void OnEscape() {
    if (g.batchBusy) { CancelBatch(); return; }
    if (g.fixPanelOn) { CloseFixPanel(); return; }
    if (g.mode == Mode::Single && !g.isolate.empty()) { SetIsolation({}); return; }
    if (g.mode == Mode::Grid) {
        if (PickedCount() > 1) { PickOnly(g.selected); Redraw(); }
        else if (g.searchActive) EndSearch(true);
        return;
    }
    Command(IDM_GRID);
}

// Right-click menu: on a thumbnail in the grid, or on the image.
void ShowContextMenu(POINT client) {
    HMENU m = CreatePopupMenu();
    if (g.mode == Mode::Grid) {
        const int i = g.grid.HitTest(client);
        if (i >= 0 && !g.items[static_cast<size_t>(i)].picked) { g.selected = i; PickOnly(i); Redraw(); }
        const int n = static_cast<int>(PickedPaths().size());
        if (n == 0) { DestroyMenu(m); return; }
        AppendMenuW(m, MF_STRING | (n == 1 && SelectedOpenable() ? 0 : MF_GRAYED), IDM_IMAGE, L"&Open\tEnter");
        std::wstring exp = n == 1 ? L"&Export\u2026\tCtrl+E" : L"&Export " + std::to_wstring(n) + L" images\u2026\tCtrl+E";
        AppendMenuW(m, MF_STRING | (g.batchBusy ? MF_GRAYED : 0), IDM_EXPORT, exp.c_str());
        AppendMenuW(m, MF_STRING | (n == 1 ? 0 : MF_GRAYED), IDM_SHOW_IN_EXPLORER, L"Show in E&xplorer");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_SELECT_ALL, L"Select &all\tCtrl+A");
    } else {
        if (!HasImage()) { DestroyMenu(m); return; }
        AppendMenuW(m, MF_STRING | (g.exportBusy || g.batchBusy ? MF_GRAYED : 0), IDM_EXPORT, L"&Export\u2026\tCtrl+E");
        AppendMenuW(m, MF_STRING | (g.pixelValid ? 0 : MF_GRAYED), IDM_COPY_PIXEL, L"&Copy pixel values\tCtrl+C");
        AppendMenuW(m, MF_STRING | (CanFix() && !g.fixBusy ? 0 : MF_GRAYED), IDM_FIX, L"&Fix HDRI\u2026\tH");
        if (FixActive()) {
            AppendMenuW(m, MF_STRING | (g.showingOriginal ? MF_CHECKED : 0), IDM_FIX_COMPARE, L"&Before / after\tB");
            AppendMenuW(m, MF_STRING | (g.exportBusy ? MF_GRAYED : 0), IDM_SAVE_FIXED, L"&Save fixed HDRI\u2026\tCtrl+S");
        }
        if (LayerCount() > 1) AppendMenuW(m, MF_STRING, IDM_LAYER_MENU, L"EXR &layer\u2026\tL");
        if (!g.isolate.empty()) AppendMenuW(m, MF_STRING, IDM_CLEAR_ISOLATE, L"&Show all objects\tEsc");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_GRID, L"Folder &grid\tG");
    }
    POINT pt = client;
    ClientToScreen(g.hwnd, &pt);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) Command(cmd);
}

void Command(UINT id) {
    const bool grid = g.mode == Mode::Grid;
    switch (id) {
        case IDM_OPEN: ShowOpenDialog(); break;
        case IDM_OPEN_FOLDER: ShowOpenFolderDialog(); break;
        case IDM_REFRESH:
            if (grid && !g.gridFolder.empty()) OpenFolder(g.gridFolder, L"");
            else if (!grid) { RefreshSiblings(TargetPath()); Redraw(); }
            break;
        case IDM_GRID: ShowGrid(); break;
        case IDM_EXPORT: if (grid) StartBatchExport(); else ShowExportDialog(); break;
        case IDM_SELECT_ALL: if (grid) PickAll(); break;
        case IDM_CLEAR_ISOLATE: SetIsolation({}); break;
        case IDM_INVERT_DEPTH: if (!grid && CurrentView() == LayerView::Depth) ToggleInvertDepth(); break;
        case IDM_THUMB_BIGGER: if (grid) SetThumbScale(g.thumbScale + 0.25f); break;
        case IDM_THUMB_SMALLER: if (grid) SetThumbScale(g.thumbScale - 0.25f); break;
        case IDM_THUMB_RESET: if (grid) SetThumbScale(1.0f); break;
        case IDM_COPY_PIXEL: if (!grid) CopyPixel(); break;
        case IDM_LAYER_PREV: if (!grid && g.doc) SwitchLayer(CurrentLayer() - 1); break;
        case IDM_LAYER_NEXT: if (!grid && g.doc) SwitchLayer(CurrentLayer() + 1); break;
        case IDM_LAYER_MENU: ShowLayerMenu(); break;
        case IDM_IMAGE: if (grid) OpenSelected(); break;
        case IDM_EXIT: SendMessageW(g.hwnd, WM_CLOSE, 0, 0); break;   // so settings are saved
        case IDM_SHORTCUTS: ShowShortcuts(); break;
        case IDM_ABOUT: ShowAbout(); break;
        case IDM_SOURCE: ShellExecuteW(g.hwnd, L"open", kSourcePage, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_SUPPORT: ShellExecuteW(g.hwnd, L"open", kDownloadPage, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_SPONSOR: ShellExecuteW(g.hwnd, L"open", kSponsorPage, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_ADD_FOLDER: ShowAddFolderDialog(); break;
        case IDM_FIND: FocusSearch(); break;
        case IDM_UNDO_REMOVE: UndoRemoveFolder(); break;
        case IDM_TOGGLE_SIDEBAR:
            g.cfg.showSidebar = !g.cfg.showSidebar;
            if (!g.cfg.showSidebar && GetFocus() == g.searchEdit) SetFocus(g.hwnd);
            SaveLibrary();
            OnResize();
            break;
        case IDM_TOGGLE_INSPECTOR:
            g.cfg.showInspector = !g.cfg.showInspector;
            SaveLibrary();
            OnResize();
            break;
        case IDM_SHOW_IN_EXPLORER:
            if (grid) { if (g.selected >= 0 && g.selected < static_cast<int>(g.items.size())) ShowInExplorer(g.items[static_cast<size_t>(g.selected)].path, true); }
            else if (!g.path.empty()) ShowInExplorer(g.path, true);
            break;
        case IDM_SUN_CHECK: if (!grid) ToggleSunCheck(); break;
        case IDM_FIX: if (g.fixPanelOn) CloseFixPanel(); else if (!grid) StartFix(); break;
        case IDM_FIX_COMPARE: if (!grid) ToggleFixCompare(); break;
        case IDM_SAVE_FIXED: if (!grid) ShowSaveFixedDialog(); break;
        case IDM_PANO:
            if (grid || !HasImage()) break;
            if (!LooksPanoramic(g.image->width, g.image->height)) {
                SetStatus(L"360\u00b0 view needs a 2:1 equirectangular image");
                break;
            }
            g.panoOn = !g.panoOn;
            g.panoFast = false;
            g.pixelValid = false;
            Redraw();
            break;
        case IDM_PANO_RESET: if (PanoActive()) ResetPanoView(); break;
        case IDM_FIT: if (PanoActive()) ResetPanoView(); else if (!grid) { SetFit(); Redraw(); } break;
        case IDM_ACTUAL: if (!grid && !PanoActive()) ZoomActualSize(ViewportCenter()); break;
        case IDM_ZOOM_IN:
            if (PanoActive()) { g.pano.fov /= 1.15; PanoMoved(); }
            else if (!grid) ZoomAround(ViewportCenter(), g.zoom * 1.25);
            else SetThumbScale(g.thumbScale + 0.25f);
            break;
        case IDM_ZOOM_OUT:
            if (PanoActive()) { g.pano.fov *= 1.15; PanoMoved(); }
            else if (!grid) ZoomAround(ViewportCenter(), g.zoom / 1.25);
            else SetThumbScale(g.thumbScale - 0.25f);
            break;
        case IDM_PREV: if (grid) GridMove(-1); else Navigate(Nav::Prev); break;
        case IDM_NEXT: if (grid) GridMove(1); else Navigate(Nav::Next); break;
        case IDM_FIRST: if (grid) SelectItem(0); else Navigate(Nav::First); break;
        case IDM_LAST: if (grid) SelectItem(static_cast<int>(g.items.size()) - 1); else Navigate(Nav::Last); break;
        case IDM_EXP_UP: SetExposure(g.settings.exposureEV + kExposureKeyStep); break;
        case IDM_EXP_DOWN: SetExposure(g.settings.exposureEV - kExposureKeyStep); break;
        case IDM_EXP_RESET: SetExposure(0.0f); RequestFullRender(); break;
        case IDM_TM_CLIP: SetCurve(Tonemap::Clip); RequestFullRender(); break;
        case IDM_TM_REINHARD: SetCurve(Tonemap::Reinhard); RequestFullRender(); break;
        case IDM_TM_ACES: SetCurve(Tonemap::ACES); RequestFullRender(); break;
        case IDM_TM_CYCLE: {
            Tonemap next = g.settings.curve == Tonemap::Clip ? Tonemap::Reinhard
                         : g.settings.curve == Tonemap::Reinhard ? Tonemap::ACES : Tonemap::Clip;
            SetCurve(next);
            RequestFullRender();
            break;
        }
    }
}

void ToolAction(ToolId t) {
    const ToolbarState st = MakeToolbarState();
    switch (t) {
        case ToolId::Grid: if (st.canGrid) Command(IDM_GRID); break;
        case ToolId::Image: if (st.gridMode && st.canImage) Command(IDM_IMAGE); break;
        case ToolId::Reset: if (st.hasImage) Command(IDM_EXP_RESET); break;
        case ToolId::Clip: if (st.hasImage) Command(IDM_TM_CLIP); break;
        case ToolId::Reinhard: if (st.hasImage) Command(IDM_TM_REINHARD); break;
        case ToolId::Aces: if (st.hasImage) Command(IDM_TM_ACES); break;
        case ToolId::Pano: if (st.viewControls) Command(IDM_PANO); break;
        case ToolId::Fix: if (st.viewControls && st.canFix) Command(IDM_FIX); break;
        case ToolId::Fit: if (st.viewControls) Command(IDM_FIT); break;
        case ToolId::Actual: if (st.viewControls) Command(IDM_ACTUAL); break;
        case ToolId::Prev: if (st.canPrev) Command(IDM_PREV); break;
        case ToolId::Next: if (st.canNext) Command(IDM_NEXT); break;
        default: break;
    }
}

// --- mouse ------------------------------------------------------------------------------------

void SetHover(ToolId t) {
    if (t == g.hover) return;
    g.hover = t;
    InvalidateToolbar();
}

bool ExposureEnabled() { return MakeToolbarState().hasImage; }

void OnSidebarClick(POINT p) {
    const SidebarHit h = g.sidebar.HitTest(p, MakeSidebarState());
    const TreeRow* row = h.row >= 0 && h.row < static_cast<int>(g.rows.size()) ? &g.rows[static_cast<size_t>(h.row)] : nullptr;
    switch (h.part) {
        case SidebarPart::Add:
        case SidebarPart::EmptyAdd: ShowAddFolderDialog(); break;
        case SidebarPart::ScopeThis:
        case SidebarPart::ScopeAll:
            g.searchAll = h.part == SidebarPart::ScopeAll;
            if (g.searchActive) RunSearch();
            RefreshTree();
            break;
        case SidebarPart::RowChevron: if (row) ToggleExpanded(row->path); break;
        case SidebarPart::RowRemove: if (row) RemoveLibraryFolder(row->root); break;
        case SidebarPart::Row:
            if (!row) break;
            if (row->missing) { SetStatus(L"This folder isn\u2019t there (an unplugged drive?). EXRay keeps looking for it."); break; }
            if (!ConfirmDiscardFix()) break;
            if (g.searchActive && GetWindowTextLengthW(g.searchEdit) > 0) SetWindowTextW(g.searchEdit, L"");
            OpenFolder(row->path, L"");
            break;
        default: break;
    }
}

void OnLButtonDown(POINT p) {
    SetFocus(g.hwnd);
    ToolId t = g.toolbar.HitTest(p);
    if (t == ToolId::Slider) {
        if (!ExposureEnabled()) return;
        g.drag = Drag::Slider;
        g.pressed = ToolId::Slider;
        SetCapture(g.hwnd);
        SetExposure(g.toolbar.ExposureFromX(p.x));
        InvalidateToolbar();
        return;
    }
    if (t != ToolId::None) {
        g.drag = Drag::Button;
        g.pressed = t;
        SetCapture(g.hwnd);
        InvalidateToolbar();
        return;
    }
    if (SidebarShown() && PtInRect(&g.sidebar.Area(), p)) { OnSidebarClick(p); return; }
    if (InspectorShown() && !g.fixPanelOn && PtInRect(&g.inspector.Area(), p)) {
        const int id = g.inspector.HitTest(p);
        if (id) {
            g.inspPressed = id;
            g.drag = Drag::Panel;
            SetCapture(g.hwnd);
            RECT r = InspectorRect();
            InvalidateRect(g.hwnd, &r, FALSE);
        }
        return;
    }
    if (g.mode == Mode::Grid && PtInRect(&g_subToggle, p)) {
        const int root = g.library.RootOf(g.gridFolder);
        if (root >= 0) SetLibrarySubfolders(root, !g.library.Folders()[static_cast<size_t>(root)].subfolders);
        return;
    }
    if (PtInRect(&g.layerChip, p)) { ShowLayerMenu(); return; }
    if (g.mode == Mode::Grid && PtInRect(&g.sizeSlider, p)) {
        g.drag = Drag::SizeSlider;
        SetCapture(g.hwnd);
        SetThumbScale(SizeFromX(p.x));
        return;
    }
    if (!InViewport(p)) return;

    if (g.mode == Mode::Grid) {
        if (g.grid.InScrollbar(p)) {
            RECT thumb = g.grid.ScrollThumbRect();
            if (p.y >= thumb.top && p.y < thumb.bottom) {
                g.drag = Drag::Scroll;
                g.grabOffset = p.y - thumb.top;
                SetCapture(g.hwnd);
            } else {
                const RECT vp = Viewport();
                const int page = std::max(1, static_cast<int>(vp.bottom - vp.top) - Scale(40));
                g.grid.ScrollBy(p.y < thumb.top ? -page : page);
            }
            Redraw();
            return;
        }
        int i = g.grid.HitTest(p);
        if (i >= 0) {
            if (CtrlDown()) {                      // add / remove one tile
                GridItem& it = g.items[static_cast<size_t>(i)];
                it.picked = !it.picked;
                g.anchor = i;
            } else if (ShiftDown()) {              // range from the anchor
                PickRange(g.anchor, i);
            } else {
                PickOnly(i);
            }
            g.selected = i;
            Redraw();
        }
        return;
    }

    if (HasImage()) {
        g.drag = Drag::Pan;
        g.downPoint = p;
        g.movedSinceDown = false;
        g.lastMouse = p;
        SetCapture(g.hwnd);
        if (CanPan()) SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }
}

void InvalidateInfoBar() {
    RECT vp = Viewport();
    RECT bar = ClientRect(); bar.top = vp.bottom;
    InvalidateRect(g.hwnd, &bar, FALSE);
}

// Reads the linear pixel values under the mouse (image view only).
void UpdatePixelReadout(POINT p) {
    bool valid = false;
    int x = 0, y = 0;
    if (g.mode == Mode::Single && HasImage() && InViewport(p) && !PlaceholderItem())
        valid = ScreenToImage(p, x, y);
    if (!valid) {
        if (g.pixelValid) { g.pixelValid = false; InvalidateInfoBar(); }
        return;
    }
    float rawv[3];
    const float* px = g.image->rgb() + (static_cast<size_t>(y) * g.image->width + x) * 3;
    if (CurrentView() != LayerView::Color && CurrentCrypto() < 0) {   // data view: show the stored values
        g.doc->RawAt(g.layer, x, y, rawv);
        px = rawv;
    }
    if (g.pixelValid && x == g.pixelX && y == g.pixelY && px[0] == g.pixel[0] && px[1] == g.pixel[1] && px[2] == g.pixel[2] &&
        CurrentCrypto() < 0)
        return;
    g.pixelValid = true;
    g.pixelX = x;
    g.pixelY = y;
    g.pixel[0] = px[0]; g.pixel[1] = px[1]; g.pixel[2] = px[2];
    g.pixelHits.clear();
    if (const int set = CurrentCrypto(); set >= 0) g.pixelHits = g.doc->CryptoAt(set, x, y);
    InvalidateInfoBar();
}

void CopyPixel() {
    if (!g.pixelValid) { MessageBeep(MB_OK); return; }
    wchar_t buf[256];
    std::swprintf(buf, std::size(buf), L"x=%d y=%d R=%.6g G=%.6g B=%.6g", g.pixelX, g.pixelY,
                  static_cast<double>(g.pixel[0]), static_cast<double>(g.pixel[1]), static_cast<double>(g.pixel[2]));
    std::wstring text = buf;
    if (const int set = CurrentCrypto(); set >= 0) {
        std::swprintf(buf, std::size(buf), L"x=%d y=%d", g.pixelX, g.pixelY);
        text = buf;
        for (const auto& h : g.pixelHits) {
            std::swprintf(buf, std::size(buf), L" (%.1f%%)", static_cast<double>(h.coverage) * 100.0);
            text += L"  " + Utf8ToWide(g.doc->CryptoName(set, h.id)) + buf;
        }
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(h), text.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    SetStatus(CurrentCrypto() >= 0 ? L"Object names copied" : L"Pixel values copied");
}

// Hover highlight in the sidebar and on the inspector's buttons.
void UpdatePanelHover(POINT p) {
    const SidebarHit sh = SidebarShown() ? g.sidebar.HitTest(p, MakeSidebarState()) : SidebarHit{};
    if (sh != g.sideHover) {
        g.sideHover = sh;
        RECT r = SidebarRect();
        InvalidateRect(g.hwnd, &r, FALSE);
    }
    const int ih = InspectorShown() && !g.fixPanelOn ? g.inspector.HitTest(p) : 0;
    if (ih != g.inspHover) {
        g.inspHover = ih;
        RECT r = InspectorRect();
        InvalidateRect(g.hwnd, &r, FALSE);
    }
}

void OnMouseMove(POINT p) {
    if (!g.trackingLeave) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g.hwnd, 0};
        TrackMouseEvent(&tme);
        g.trackingLeave = true;
    }
    switch (g.drag) {
        case Drag::Slider:
            SetExposure(g.toolbar.ExposureFromX(p.x));
            if (g.mode == Mode::Single) SetTimer(g.hwnd, kRenderTimer, kRenderDelayMs, nullptr);
            return;
        case Drag::Pan:
            if (PanoActive()) {
                // Grab-and-drag: the scene follows the mouse.
                RECT vp = Viewport();
                const double vw = std::max(1L, vp.right - vp.left), vh = std::max(1L, vp.bottom - vp.top);
                const double degPerPx = g.pano.fov / vw;
                const double vfov = 2.0 * std::atan(std::tan(g.pano.fov * 0.5 * 3.14159265358979 / 180.0) * vh / vw) * 180.0 / 3.14159265358979;
                g.pano.yaw -= (p.x - g.lastMouse.x) * degPerPx;
                g.pano.pitch += (p.y - g.lastMouse.y) * vfov / vh;
                PanoMoved();
            } else if (CanPan()) {
                g.cx -= (p.x - g.lastMouse.x) / g.zoom;
                g.cy -= (p.y - g.lastMouse.y) / g.zoom;
                g.fit = false;
                ClampView();
                Redraw();
            }
            g.lastMouse = p;
            if (std::abs(p.x - g.downPoint.x) > Scale(3) || std::abs(p.y - g.downPoint.y) > Scale(3))
                g.movedSinceDown = true;
            UpdatePixelReadout(p);
            return;
        case Drag::SizeSlider:
            SetThumbScale(SizeFromX(p.x));
            return;
        case Drag::Scroll:
            g.grid.DragThumbTo(p.y - g.grabOffset);
            Redraw();
            return;
        case Drag::Button:
        case Drag::Panel:
            return;   // keep the pressed look; the action happens on release
        case Drag::None:
            SetHover(g.toolbar.HitTest(p));
            UpdatePanelHover(p);
            UpdatePixelReadout(p);
            if (const bool hot = PtInRect(&g.layerChip, p) != FALSE; hot != g.chipHot) {
                g.chipHot = hot;
                InvalidateInfoBar();
            }
            if (const bool hot = g.mode == Mode::Grid && PtInRect(&g.sizeSlider, p); hot != g.sizeHot) {
                g.sizeHot = hot;
                InvalidateInfoBar();
            }
            if (g.mode == Mode::Grid) {
                const int h = g.grid.HitTest(p);
                const bool sh = g.grid.InScrollbar(p);
                if (h != g.gridHover || sh != g.scrollHot) {
                    g.gridHover = h;
                    g.scrollHot = sh;
                    RECT vp = Viewport();
                    RECT r = ClientRect(); r.top = vp.top;   // grid + info bar
                    InvalidateRect(g.hwnd, &r, FALSE);
                }
            }
            return;
    }
}

void EndDrag() {
    if (g.drag == Drag::Slider) RequestFullRender();
    if (g.drag == Drag::Panel) { g.inspPressed = 0; RECT r = InspectorRect(); InvalidateRect(g.hwnd, &r, FALSE); }
    if (g.drag == Drag::Scroll || g.drag == Drag::SizeSlider) Redraw();
    g.drag = Drag::None;
    if (g.pressed != ToolId::None) { g.pressed = ToolId::None; InvalidateToolbar(); }
}

void OnLButtonUp(POINT p) {
    const Drag d = g.drag;
    const ToolId pressed = g.pressed;
    const int panelPressed = g.inspPressed;
    EndDrag();
    if (d == Drag::Panel) {
        if (GetCapture() == g.hwnd) ReleaseCapture();
        if (panelPressed && g.inspector.HitTest(p) == panelPressed) Command(static_cast<UINT>(panelPressed));
        return;
    }
    if (GetCapture() == g.hwnd) ReleaseCapture();
    if (d == Drag::Button && g.toolbar.HitTest(p) == pressed) ToolAction(pressed);
    if (d == Drag::Pan && !g.movedSinceDown && CurrentCrypto() >= 0) CryptoClick(p, CtrlDown());
    SetHover(g.toolbar.HitTest(p));
}

void OnDoubleClick(POINT p) {
    if (SidebarShown() && PtInRect(&g.sidebar.Area(), p)) {   // a second quick click: open / fold again
        const SidebarHit h = g.sidebar.HitTest(p, MakeSidebarState());
        if (h.part == SidebarPart::Row && h.row >= 0 && g.rows[static_cast<size_t>(h.row)].hasChildren)
            ToggleExpanded(g.rows[static_cast<size_t>(h.row)].path);
        else if (h.part != SidebarPart::Row) OnSidebarClick(p);
        return;
    }
    if (InspectorShown() && !g.fixPanelOn && PtInRect(&g.inspector.Area(), p)) { OnLButtonDown(p); return; }
    ToolId t = g.toolbar.HitTest(p);
    if (t == ToolId::Slider) {
        if (ExposureEnabled()) { SetExposure(0.0f); RequestFullRender(); }
        return;
    }
    if (t != ToolId::None) { OnLButtonDown(p); return; }   // a quick second click on a button
    if (g.mode == Mode::Grid && PtInRect(&g.sizeSlider, p)) { SetThumbScale(1.0f); return; }
    if (!InViewport(p)) return;

    if (g.mode == Mode::Grid) {
        if (g.grid.InScrollbar(p)) { OnLButtonDown(p); return; }
        int i = g.grid.HitTest(p);
        if (i >= 0 && !CtrlDown() && !ShiftDown()) { g.selected = i; PickOnly(i); OpenSelected(); }
        else if (i >= 0) OnLButtonDown(p);
        return;
    }
    if (!HasImage()) return;
    if (PanoActive()) { ResetPanoView(); return; }
    if (CurrentCrypto() >= 0) return;   // on Cryptomatte views a click picks objects; no zoom toggle
    if (g.fit && FitZoom() < 1.0) ZoomActualSize(p);
    else { SetFit(); Redraw(); }
}

void OnMouseWheel(POINT screenPt, int delta) {
    POINT p = screenPt;
    ScreenToClient(g.hwnd, &p);
    const double notches = delta / static_cast<double>(WHEEL_DELTA);
    if (SidebarShown() && PtInRect(&g.sidebar.Area(), p)) {
        if (g.sidebar.ScrollBy(-static_cast<int>(std::lround(notches * 3 * g.sidebar.RowHeight())))) {
            UpdatePanelHover(p);
            RECT r = SidebarRect();
            InvalidateRect(g.hwnd, &r, FALSE);
        }
        return;
    }
    if (g.fixPanelOn && g.fixPanel && PtInRect(&g.inspector.Area(), p)) {
        SendMessageW(g.fixPanel, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))),
                     MAKELPARAM(screenPt.x, screenPt.y));
        return;
    }
    if (g.toolbar.HitTest(p) == ToolId::Slider) {
        if (!ExposureEnabled()) return;
        SetExposure(g.settings.exposureEV + static_cast<float>(0.1 * notches));
        if (g.mode == Mode::Single) SetTimer(g.hwnd, kRenderTimer, kRenderDelayMs, nullptr);
        return;
    }
    if (g.mode == Mode::Grid && (PtInRect(&g.sizeSlider, p) || (CtrlDown() && InViewport(p)))) {
        SetThumbScale(g.thumbScale + static_cast<float>(0.1 * notches));
        return;
    }
    if (!InViewport(p)) return;
    if (g.mode == Mode::Grid) {
        g.grid.ScrollBy(-static_cast<int>(std::lround(notches * g.grid.WheelStep())));
        g.gridHover = g.grid.HitTest(p);
        Redraw();
        return;
    }
    if (PanoActive()) {
        g.pano.fov *= std::pow(1.0 / 1.15, notches);   // wheel up = zoom in (narrower view)
        PanoMoved();
    } else if (HasImage()) {
        ZoomAround(p, g.zoom * std::pow(1.25, notches));
    }
    UpdatePixelReadout(p);
}

bool OnSetCursor() {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g.hwnd, &p);
    if (PtInRect(&g.layerChip, p)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return true; }
    if ((g.sideHover.part != SidebarPart::None && SidebarShown() && PtInRect(&g.sidebar.Area(), p)) ||
        (g.inspHover && InspectorShown() && !g.fixPanelOn && PtInRect(&g.inspector.Area(), p)) ||
        (g.mode == Mode::Grid && PtInRect(&g_subToggle, p))) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return true;
    }
    if (g.mode == Mode::Grid) return false;
    if (g.drag == Drag::Pan && CanPan()) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return true; }
    if (InViewport(p) && PanoActive()) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return true; }
    if (InViewport(p) && CanPan()) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return true; }
    return false;
}

// --- keyboard ---------------------------------------------------------------------------------

bool OnGridKey(WPARAM vk) {
    const int cols = g.grid.Columns();
    const int n = static_cast<int>(g.items.size());
    switch (vk) {
        case VK_LEFT: GridMove(-1); return true;
        case VK_RIGHT: GridMove(1); return true;
        case VK_UP: if (g.selected - cols >= 0) GridMove(-cols); return true;
        case VK_DOWN:
            if (g.selected + cols < n) GridMove(cols);
            else if (g.selected / cols < (n - 1) / cols) SelectItem(n - 1, ShiftDown());   // short last row
            return true;
        case VK_PRIOR: GridMove(-cols * g.grid.RowsPerPage()); return true;
        case VK_NEXT: GridMove(cols * g.grid.RowsPerPage()); return true;
        case VK_HOME: SelectItem(0, ShiftDown()); return true;
        case VK_END: SelectItem(n - 1, ShiftDown()); return true;
        case VK_RETURN: OpenSelected(); return true;
    }
    return false;
}

bool OnKeyDown(WPARAM vk) {
    const bool ctrl = CtrlDown();
    if (g.mode == Mode::Grid && !ctrl && OnGridKey(vk)) return true;
    switch (vk) {
        case VK_LEFT: case VK_PRIOR: Command(IDM_PREV); return true;
        case VK_RIGHT: case VK_NEXT: Command(IDM_NEXT); return true;
        case VK_HOME: Command(IDM_FIRST); return true;
        case VK_END: Command(IDM_LAST); return true;
        case VK_ESCAPE: OnEscape(); return true;
        case VK_BACK: Command(IDM_GRID); return true;
        case 'A': if (ctrl) { Command(IDM_SELECT_ALL); return true; } break;
        case 'Z': if (ctrl) { Command(IDM_UNDO_REMOVE); return true; } break;
        case '0': case VK_NUMPAD0: if (ctrl) { Command(IDM_THUMB_RESET); return true; } break;
        case VK_F5: Command(IDM_REFRESH); return true;
        case VK_F1: Command(IDM_SHORTCUTS); return true;
        case VK_ADD: case VK_OEM_PLUS: Command(ctrl ? IDM_ZOOM_IN : IDM_EXP_UP); return true;
        case VK_SUBTRACT: case VK_OEM_MINUS: Command(ctrl ? IDM_ZOOM_OUT : IDM_EXP_DOWN); return true;
        case 'G': if (!ctrl) { Command(IDM_GRID); return true; } break;
        case 'E': if (ctrl) { Command(IDM_EXPORT); return true; } break;
        case 'C': if (ctrl) { Command(IDM_COPY_PIXEL); return true; } break;
        case 'L': if (!ctrl) { Command(IDM_LAYER_MENU); return true; } break;
        case 'I': Command(ctrl ? IDM_TOGGLE_INSPECTOR : IDM_INVERT_DEPTH); return true;
        case 'V': if (!ctrl) { Command(IDM_PANO); return true; } break;
        case 'H': if (!ctrl) { Command(IDM_FIX); return true; } break;
        case 'B': Command(ctrl ? IDM_TOGGLE_SIDEBAR : IDM_FIX_COMPARE); return true;
        case 'S': Command(ctrl ? IDM_SAVE_FIXED : IDM_SUN_CHECK); return true;
        case VK_OEM_4: Command(IDM_LAYER_PREV); return true;   // [
        case VK_OEM_6: Command(IDM_LAYER_NEXT); return true;   // ]
        case 'R': if (!ctrl) { Command(IDM_EXP_RESET); return true; } break;
        case 'T': if (!ctrl) { Command(IDM_TM_CYCLE); return true; } break;
        case 'F': Command(ctrl ? IDM_FIND : IDM_FIT); return true;
        case '1': case VK_NUMPAD1: if (!ctrl) { Command(IDM_ACTUAL); return true; } break;
    }
    return false;
}

// --- window -----------------------------------------------------------------------------------

HMENU BuildMenu() {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open file\u2026\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_OPEN_FOLDER, L"Open &folder\u2026\tCtrl+Shift+O");
    AppendMenuW(file, MF_STRING, IDM_ADD_FOLDER, L"&Add folder to the library\u2026");
    AppendMenuW(file, MF_STRING, IDM_REFRESH, L"&Refresh folder\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXPORT, L"&Export as PNG or JPEG\u2026\tCtrl+E");
    AppendMenuW(file, MF_STRING, IDM_SAVE_FIXED, L"&Save fixed HDRI\u2026\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SELECT_ALL, L"Select &all in grid\tCtrl+A");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PREV, L"&Previous\t\u2190");
    AppendMenuW(file, MF_STRING, IDM_NEXT, L"&Next\t\u2192");
    AppendMenuW(file, MF_STRING, IDM_FIRST, L"F&irst\tHome");
    AppendMenuW(file, MF_STRING, IDM_LAST, L"&Last\tEnd");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_SIDEBAR, L"F&olders panel\tCtrl+B");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_INSPECTOR, L"I&nspector\tCtrl+I");
    AppendMenuW(view, MF_STRING, IDM_FIND, L"Searc&h files\tCtrl+F");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_GRID, L"Folder &grid\tG");
    AppendMenuW(view, MF_STRING, IDM_IMAGE, L"Open selected &image\tEnter");
    AppendMenuW(view, MF_STRING, IDM_THUMB_BIGGER, L"&Bigger thumbnails\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_THUMB_SMALLER, L"&Smaller thumbnails\tCtrl+\u2212");
    AppendMenuW(view, MF_STRING, IDM_THUMB_RESET, L"Thumbnails at 100%\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_PANO, L"&360\u00b0 view\tV");
    AppendMenuW(view, MF_STRING, IDM_PANO_RESET, L"Reset 360\u00b0 view");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_FIT, L"&Fit to window\tF");
    AppendMenuW(view, MF_STRING, IDM_ACTUAL, L"&100%\t1");
    AppendMenuW(view, MF_STRING, IDM_ZOOM_IN, L"Zoom &in\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_ZOOM_OUT, L"Zoom &out\tCtrl+\u2212");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_EXP_UP, L"Exposure &up\t+");
    AppendMenuW(view, MF_STRING, IDM_EXP_DOWN, L"Exposure &down\t\u2212");
    AppendMenuW(view, MF_STRING, IDM_EXP_RESET, L"&Reset exposure\tR");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_TM_CLIP, L"Tonemap: &Clip");
    AppendMenuW(view, MF_STRING, IDM_TM_REINHARD, L"Tonemap: R&einhard");
    AppendMenuW(view, MF_STRING, IDM_TM_ACES, L"Tonemap: &ACES");
    AppendMenuW(view, MF_STRING, IDM_TM_CYCLE, L"&Next tonemap\tT");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_LAYER_MENU, L"EXR &layer\u2026\tL");
    AppendMenuW(view, MF_STRING, IDM_LAYER_PREV, L"Previous layer\t[");
    AppendMenuW(view, MF_STRING, IDM_LAYER_NEXT, L"Next layer\t]");
    AppendMenuW(view, MF_STRING, IDM_CLEAR_ISOLATE, L"Show all Cryptomatte objects\tEsc");
    AppendMenuW(view, MF_STRING, IDM_INVERT_DEPTH, L"&Invert depth view\tI");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_COPY_PIXEL, L"&Copy pixel values\tCtrl+C");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_FIX, L"&Fix HDRI\u2026\tH");
    AppendMenuW(tools, MF_STRING, IDM_FIX_COMPARE, L"&Before / after the fix\tB");
    AppendMenuW(tools, MF_STRING, IDM_SUN_CHECK, L"Sun &check (is the sun clipped?)\tS");
    AppendMenuW(tools, MF_STRING, IDM_SAVE_FIXED, L"&Save fixed HDRI\u2026\tCtrl+S");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_SHORTCUTS, L"&Controls and shortcuts\tF1");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_SOURCE, L"Source code on &GitHub");
    AppendMenuW(help, MF_STRING, IDM_SPONSOR, L"S&ponsor on GitHub\u2026");
    AppendMenuW(help, MF_STRING, IDM_SUPPORT, L"&Support on Gumroad\u2026");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

void UpdateMenuChecks(HMENU menu) {
    const ToolbarState st = MakeToolbarState();
    auto enable = [&](UINT id, bool on) { EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    for (UINT id : {IDM_FIT, IDM_ACTUAL, IDM_ZOOM_IN, IDM_ZOOM_OUT}) enable(id, st.viewControls);
    if (st.gridMode) { enable(IDM_ZOOM_IN, g.thumbScale < 2.0f); enable(IDM_ZOOM_OUT, g.thumbScale > 0.5f); }
    for (UINT id : {IDM_EXP_UP, IDM_EXP_DOWN, IDM_EXP_RESET, IDM_TM_CLIP, IDM_TM_REINHARD, IDM_TM_ACES, IDM_TM_CYCLE})
        enable(id, st.hasImage);
    enable(IDM_PREV, st.canPrev);
    enable(IDM_NEXT, st.canNext);
    const bool anyFiles = st.gridMode ? !g.items.empty() : CurrentNav().count > 0;
    enable(IDM_FIRST, anyFiles);
    enable(IDM_LAST, anyFiles);
    enable(IDM_GRID, st.canGrid && !st.gridMode);
    enable(IDM_IMAGE, st.gridMode && st.canImage);
    enable(IDM_REFRESH, st.gridMode ? !g.gridFolder.empty() : !TargetPath().empty());
    enable(IDM_EXPORT, (st.viewControls || (st.gridMode && !PickedPaths().empty())) && !g.exportBusy && !g.batchBusy);
    enable(IDM_SELECT_ALL, st.gridMode && !g.items.empty());
    enable(IDM_CLEAR_ISOLATE, !st.gridMode && !g.isolate.empty());
    enable(IDM_INVERT_DEPTH, !st.gridMode && CurrentView() == LayerView::Depth);
    enable(IDM_PANO, st.canPano);
    enable(IDM_PANO_RESET, st.pano);
    enable(IDM_ACTUAL, st.viewControls && !st.pano);
    CheckMenuItem(menu, IDM_PANO, MF_BYCOMMAND | (st.pano ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_INVERT_DEPTH, MF_BYCOMMAND | (g.viewOpts.invertDepth ? MF_CHECKED : MF_UNCHECKED));
    enable(IDM_THUMB_BIGGER, st.gridMode && g.thumbScale < 2.0f);
    enable(IDM_THUMB_SMALLER, st.gridMode && g.thumbScale > 0.5f);
    enable(IDM_THUMB_RESET, st.gridMode);
    const bool layers = st.viewControls && LayerCount() > 1;
    enable(IDM_LAYER_MENU, layers);
    enable(IDM_LAYER_PREV, layers && CurrentLayer() > 0);
    enable(IDM_LAYER_NEXT, layers && CurrentLayer() + 1 < LayerCount());
    enable(IDM_COPY_PIXEL, st.viewControls && g.pixelValid);
    enable(IDM_FIX, st.viewControls && CanFix() && !g.fixBusy);
    enable(IDM_FIX_COMPARE, st.viewControls && FixActive() && !g.fixBusy);
    CheckMenuItem(menu, IDM_FIX_COMPARE, MF_BYCOMMAND | (FixActive() && g.showingOriginal ? MF_CHECKED : MF_UNCHECKED));
    enable(IDM_SAVE_FIXED, st.viewControls && FixActive() && !g.exportBusy && !g.batchBusy);
    enable(IDM_SUN_CHECK, st.viewControls && !ViewIsData());
    CheckMenuItem(menu, IDM_SUN_CHECK, MF_BYCOMMAND | (g.sunCheck.on ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(menu, IDM_TM_CLIP, IDM_TM_ACES,
                       g.settings.curve == Tonemap::Clip ? IDM_TM_CLIP
                       : g.settings.curve == Tonemap::Reinhard ? IDM_TM_REINHARD : IDM_TM_ACES,
                       MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_FIT, MF_BYCOMMAND | (g.fit && st.viewControls ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_TOGGLE_SIDEBAR, MF_BYCOMMAND | (g.cfg.showSidebar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_TOGGLE_INSPECTOR, MF_BYCOMMAND | (g.cfg.showInspector ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_FIX, MF_BYCOMMAND | (g.fixPanelOn ? MF_CHECKED : MF_UNCHECKED));
    enable(IDM_FIX, (st.viewControls && CanFix() && !g.fixBusy) || g.fixPanelOn);
}

// The search box and the Fix HDRI panel are child windows placed over the panels.
void PlaceChildren() {
    if (g.searchEdit) {
        if (SidebarShown()) {
            const RECT b = g.sidebar.SearchBoxRect();
            const int h = Scale(20);
            SetWindowPos(g.searchEdit, nullptr, b.left + Scale(30), (b.top + b.bottom - h) / 2, (b.right - b.left) - Scale(38), h,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            if (GetFocus() == g.searchEdit) SetFocus(g.hwnd);
            ShowWindow(g.searchEdit, SW_HIDE);
        }
    }
    if (g.fixPanel) {
        if (g.fixPanelOn) {
            const RECT r = InspectorRect();
            g.inspector.Layout(r, g.dpi);
            SetWindowPos(g.fixPanel, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g.fixPanel, SW_HIDE);
        }
    }
}

void OnResize() {
    RECT r = ClientRect();
    g.toolbar.Layout(r.right, g.dpi);
    g.sidebar.Layout(SidebarRect(), g.dpi, static_cast<int>(g.rows.size()));
    g.inspector.Layout(InspectorRect(), g.dpi);
    PlaceChildren();
    if (g.fit) SetFit();
    else ClampView();
    LayoutGrid();
    Redraw();
}

void SaveWindowSettings() {
    WINDOWPLACEMENT wpl = {};
    wpl.length = sizeof(wpl);
    if (GetWindowPlacement(g.hwnd, &wpl)) {
        const RECT& r = wpl.rcNormalPosition;
        g.cfg.hasWindow = true;
        g.cfg.winX = r.left;
        g.cfg.winY = r.top;
        g.cfg.winW = r.right - r.left;
        g.cfg.winH = r.bottom - r.top;
        g.cfg.maximized = wpl.showCmd == SW_SHOWMAXIMIZED;
    }
    g.cfg.thumbScale = g.thumbScale;
    g.cfg.exposureEV = g.sunCheck.on ? g.sunCheck.look.exposureEV : g.settings.exposureEV;
    g.cfg.curve = g.settings.curve;
    g.cfg.libraryFolders = g.library.Save();
    g.cfg.expandedFolders.assign(g.expanded.begin(), g.expanded.end());
    SaveSettings(g.cfg);
}

// --- dark menu bar ---------------------------------------------------------------------------
//
// Windows has no official dark menu bar. Popup menus follow the app's
// preferred theme mode (uxtheme ordinals 135/136, available since Windows 10
// 1903 and used by many apps); the bar itself is painted here through the
// undocumented UAH messages. If any of this is missing, menus stay light.

constexpr UINT WM_UAHDRAWMENU     = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
union UAHMENUITEMMETRICS {
    struct { DWORD cx, cy; } rgsizeBar[2];
    struct { DWORD cx, cy; } rgsizePopup[4];
};
struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

const COLORREF kMenuHot     = RGB(0x3a, 0x3a, 0x3a);
const COLORREF kMenuPressed = RGB(0x46, 0x46, 0x46);

void EnableDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(2 /* ForceDark */);
    if (flush) flush();
}

void FillColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void DrawMenuBarBackground(HWND hwnd, const UAHMENU* m) {
    MENUBARINFO mbi = {};
    mbi.cbSize = sizeof(mbi);
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
    RECT wr;
    GetWindowRect(hwnd, &wr);
    RECT r = mbi.rcBar;
    OffsetRect(&r, -wr.left, -wr.top);
    FillColor(m->hdc, r, kBarColor);
}

void DrawMenuBarItem(const UAHDRAWMENUITEM* d) {
    wchar_t text[256] = {};
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = text;
    mii.cch = static_cast<UINT>(std::size(text) - 1);
    GetMenuItemInfoW(d->um.hmenu, static_cast<UINT>(d->umi.iPosition), TRUE, &mii);
    const UINT st = d->dis.itemState;
    COLORREF bg = kBarColor, fg = kTextColor;
    if (st & ODS_HOTLIGHT) bg = kMenuHot;
    if (st & ODS_SELECTED) bg = kMenuPressed;
    if (st & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) fg = kDimText;
    RECT r = d->dis.rcItem;
    FillColor(d->um.hdc, r, bg);
    SetBkMode(d->um.hdc, TRANSPARENT);
    SetTextColor(d->um.hdc, fg);
    DrawTextW(d->um.hdc, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((st & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
}

// Windows draws a light 1-pixel line under the menu bar; paint over it.
void DrawMenuBarBottomLine(HWND hwnd) {
    if (!GetMenu(hwnd)) return;
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
    RECT line = {cr.left - wr.left, cr.top - wr.top - 1, cr.right - wr.left, cr.top - wr.top};
    HDC dc = GetWindowDC(hwnd);
    FillColor(dc, line, kBarColor);
    ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g.hwnd = hwnd;
            g.dpi = GetDpiForWindow(hwnd);
            g.grid.SetDpi(g.dpi, g.thumbScale);
            RecreateFonts();
            DragAcceptFiles(hwnd, TRUE);
            g.editBrush = CreateSolidBrush(panel::kButton);
            g.searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(4000)), GetModuleHandleW(nullptr), nullptr);
            if (g.searchEdit) {
                SendMessageW(g.searchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), FALSE);
                SendMessageW(g.searchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search files   Ctrl+F"));
                g.searchEditProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(g.searchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc)));
            }
            g.fixPanel = CreateFixPanel(hwnd);
            RefreshTree();
            OnResize();
            return 0;

        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            g.grid.SetDpi(g.dpi, g.thumbScale);
            RecreateFonts();
            if (g.searchEdit) SendMessageW(g.searchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
            if (g.fixPanel) FixPanelDpiChanged(g.fixPanel);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            OnResize();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
            RECT r = {0, 0, Toolbar::MinClientWidth(dpi), MulDiv(360, static_cast<int>(dpi), 96)};
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;   // everything is painted in WM_PAINT

        case WM_UAHDRAWMENU:
            DrawMenuBarBackground(hwnd, reinterpret_cast<const UAHMENU*>(lp));
            return TRUE;
        case WM_UAHDRAWMENUITEM:
            DrawMenuBarItem(reinterpret_cast<const UAHDRAWMENUITEM*>(lp));
            return TRUE;
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            DrawMenuBarBottomLine(hwnd);
            return r;
        }

        case WM_SIZE:
            OnResize();
            return 0;

        case WM_PAINT:
            OnPaint(hwnd);
            return 0;

        case WM_LBUTTONDOWN:
            OnLButtonDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONDBLCLK:
            OnDoubleClick({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSELEAVE:
            g.trackingLeave = false;
            SetHover(ToolId::None);
            if (g.sideHover.part != SidebarPart::None || g.inspHover) {
                g.sideHover = SidebarHit{};
                g.inspHover = 0;
                Redraw();
            }
            if (g.gridHover >= 0 || g.scrollHot || g.pixelValid || g.chipHot) {
                g.gridHover = -1; g.scrollHot = false; g.pixelValid = false; g.chipHot = false;
                Redraw();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd) EndDrag();
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && OnSetCursor()) return TRUE;
            break;

        case WM_KEYDOWN:
            if (OnKeyDown(wp)) return 0;
            break;

        case WM_TIMER:
            if (wp == kPanoSharpTimer) {
                if (g.drag == Drag::Pan) return 0;   // still moving
                KillTimer(hwnd, kPanoSharpTimer);
                g.panoFast = false;
                Redraw();
                return 0;
            }
            if (wp == kThumbSizeTimer) {
                if (g.drag == Drag::SizeSlider) return 0;   // wait until the slider is let go
                RemakeThumbsIfNeeded();
                return 0;
            }
            if (wp == kSearchTimer) { RunSearch(); return 0; }
            if (wp == kLibraryTimer) { OnLibraryChanged(); return 0; }
            if (wp == kLibCheckTimer) { CheckMissingFolders(); return 0; }
            if (wp == kStatusTimer) {
                KillTimer(hwnd, kStatusTimer);
                g.status.clear();
                Redraw();
                return 0;
            }
            if (wp == kRenderTimer) {
                if (g.drag == Drag::Slider) {
                    // Still dragging: render only if the slider has stopped moving.
                    KillTimer(hwnd, kRenderTimer);
                    RequestFullRender();
                } else {
                    RequestFullRender();
                }
                return 0;
            }
            break;

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            UINT len = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path;
            if (len > 0) {
                path.assign(len, L'\0');
                DragQueryFileW(drop, 0, path.data(), len + 1);
            }
            DragFinish(drop);
            if (!path.empty()) OpenPath(path);
            return 0;
        }

        case WM_INITMENUPOPUP:
            UpdateMenuChecks(GetMenu(hwnd));
            return 0;

        case WM_COMMAND:
            if (lp && reinterpret_cast<HWND>(lp) == g.searchEdit) {
                if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, kSearchTimer, 250, nullptr);
                return 0;
            }
            Command(LOWORD(wp));
            return 0;

        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lp) == g.searchEdit && g.editBrush) {
                HDC dc = reinterpret_cast<HDC>(wp);
                SetTextColor(dc, panel::kText);
                SetBkColor(dc, panel::kButton);
                return reinterpret_cast<LRESULT>(g.editBrush);
            }
            break;

        case WM_APP_FIXPANEL:
            OnFixPanel(static_cast<FixWindowResult>(wp));
            return 0;
        case WM_APP_SCANNED:
            OnScanned(reinterpret_cast<ScanDone*>(lp));
            return 0;
        case WM_APP_LIBCHANGED:
            SetTimer(hwnd, kLibraryTimer, 1000, nullptr);
            return 0;

        case WM_APP_LOADED:
            OnLoaded(reinterpret_cast<LoadResult*>(lp));
            return 0;
        case WM_APP_RENDERED:
            OnRendered(reinterpret_cast<RenderResult*>(lp));
            return 0;
        case WM_APP_THUMB:
            OnThumb(reinterpret_cast<ThumbResult*>(lp));
            return 0;
        case WM_APP_LAYER:
            OnLayerLoaded(reinterpret_cast<LoadResult*>(lp));
            return 0;
        case WM_APP_EXPORTED:
            OnExported(reinterpret_cast<ExportResult*>(lp));
            return 0;
        case WM_APP_ANALYZED:
            OnAnalyzed(reinterpret_cast<AnalyzeResult*>(lp));
            return 0;
        case WM_APP_FIXED:
            OnFixed(reinterpret_cast<FixedResult*>(lp));
            return 0;
        case WM_APP_BATCH_PROGRESS:
            g.batchDone = static_cast<int>(wp);
            g.batchTotal = static_cast<int>(lp);
            InvalidateInfoBar();
            return 0;
        case WM_APP_BATCH_DONE:
            OnBatchDone(reinterpret_cast<BatchResult*>(lp));
            return 0;
        case WM_CONTEXTMENU: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p.x == -1 && p.y == -1) {   // from the keyboard (menu key / Shift+F10)
                if (g.mode == Mode::Grid && g.selected >= 0) {
                    RECT r = g.grid.ItemRect(g.selected);
                    p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                } else {
                    p = ViewportCenter();
                }
            } else {
                ScreenToClient(hwnd, &p);
            }
            if (SidebarShown() && PtInRect(&g.sidebar.Area(), p)) {
                const SidebarHit h = g.sidebar.HitTest(p, MakeSidebarState());
                if (h.row >= 0) ShowFolderMenu(h.row, p);
                return 0;
            }
            if (InViewport(p)) { ShowContextMenu(p); return 0; }
            break;
        }
        case WM_CLOSE:
            if (!ConfirmDiscardFix()) return 0;
            SaveWindowSettings();
            break;

        case WM_DESTROY:
            KillTimer(hwnd, kRenderTimer);
            if (g.font) DeleteObject(g.font);
            if (g.bigFont) DeleteObject(g.bigFont);
            for (HFONT f : {g.boldFont, g.smallFont, g.titleFont}) if (f) DeleteObject(f);
            if (g.editBrush) DeleteObject(g.editBrush);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // for the folder picker

    EnableDarkMenus();   // before any window or menu exists
    g.cfg = LoadSettings();
    g.thumbScale = g.cfg.thumbScale;
    g.settings.exposureEV = g.cfg.exposureEV;
    g.settings.curve = g.cfg.curve;
    g.library.Load(g.cfg.libraryFolders);
    for (const auto& e : g.cfg.expandedFolders) g.expanded.insert(PathKey(e));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // First window: 1280x800 at the monitor's scaling.
    UINT sysDpi = GetDpiForSystem();
    int w = MulDiv(1280, static_cast<int>(sysDpi), 96);
    int h = MulDiv(800, static_cast<int>(sysDpi), 96);

    HWND hwnd = CreateWindowExW(0, kClassName, AppTitle().c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, BuildMenu(), inst, nullptr);
    if (!hwnd) return 1;

    // Dark title bar to match the dark window (Windows 11; ignored elsewhere).
    BOOL dark = TRUE;
    using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto fn = reinterpret_cast<DwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute")))
            fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }

    // Thumbnail workers: 2, or 3 on machines with 8+ cores. Each decode is
    // itself multi-threaded, and big images need a lot of memory.
    const int workers = std::thread::hardware_concurrency() >= 8 ? 3 : 2;
    g.pool = new ThumbPool(workers, [hwnd](std::unique_ptr<ThumbResult> r) {
        if (PostMessageW(hwnd, WM_APP_THUMB, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
    });

    // Window size and position from last time, if it's still on a monitor.
    bool placed = false;
    if (g.cfg.hasWindow) {
        RECT r = {g.cfg.winX, g.cfg.winY, g.cfg.winX + g.cfg.winW, g.cfg.winY + g.cfg.winH};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wpl = {};
            wpl.length = sizeof(wpl);
            wpl.rcNormalPosition = r;
            wpl.showCmd = (g.cfg.maximized && show != SW_SHOWMINIMIZED && show != SW_MINIMIZE) ? SW_SHOWMAXIMIZED : show;
            placed = SetWindowPlacement(hwnd, &wpl) != FALSE;
        }
    }
    if (!placed) ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // The folder library: read the folders in the background and watch them.
    ScanAll();
    RestartWatcher();
    SetTimer(hwnd, kLibCheckTimer, 10000, nullptr);

    // A file or folder passed on the command line (or dropped onto the .exe);
    // otherwise the folder that was open last time.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool opened = false;
    if (argv) {
        if (argc > 1) { OpenPath(argv[1]); opened = true; }
        LocalFree(argv);
    }
    if (!opened && !g.cfg.lastFolder.empty() && IsFolder(g.cfg.lastFolder)) OpenFolder(g.cfg.lastFolder, L"");

    ACCEL accel[] = { {FVIRTKEY | FCONTROL, 'O', IDM_OPEN},
                      {FVIRTKEY | FCONTROL | FSHIFT, 'O', IDM_OPEN_FOLDER},
                      {FVIRTKEY | FCONTROL, 'E', IDM_EXPORT},
                      {FVIRTKEY | FCONTROL, 'S', IDM_SAVE_FIXED},
                      {FVIRTKEY | FCONTROL, 'A', IDM_SELECT_ALL} };
    HACCEL hAccel = CreateAcceleratorTableW(accel, static_cast<int>(std::size(accel)));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        // While typing in the search box, its keys (Ctrl+A, Ctrl+C...) are its own.
        if (GetFocus() == g.searchEdit || !TranslateAcceleratorW(hwnd, hAccel, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DestroyAcceleratorTable(hAccel);
    // Leave the thumbnail workers to the system: waiting for a 16K decode to
    // finish would only delay closing the window.
    ExitProcess(static_cast<UINT>(m.wParam));
}
