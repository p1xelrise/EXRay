// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The "Fix HDRI" panel: what the analysis found, which fixes to apply and
// how the clipped sun is rebuilt.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "hdri_fix.h"

enum class FixWindowResult {
    Cancel,     // closed without doing anything
    Apply,      // "Apply fixes"
    SunCheck    // "Sun check": look at the sun in the 360 view at low exposure
};

// The parent window receives this when a footer button is clicked;
// wParam is the FixWindowResult (Cancel = "Close").
constexpr UINT WM_APP_FIXPANEL = WM_APP + 20;

// Fix HDRI as a panel docked in the main window: a child window the main
// window positions over its inspector. Created hidden.
HWND CreateFixPanel(HWND parent);

// Shows a report in the panel; `opts` are the initial choices (fixes that
// don't apply are switched off).
void FixPanelShow(HWND panel, const std::wstring& fileName, const HdriReport& report, const HdriFixOptions& opts);
void FixPanelDpiChanged(HWND panel);

// The choices as they are now in the panel.
HdriFixOptions FixPanelOptions(HWND panel);
