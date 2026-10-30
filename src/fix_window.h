// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The "Fix HDRI" window: what the analysis found, which fixes to apply and
// how strong the rebuilt sun should be.
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

// Shows the report modally over `owner`. `opts` holds the initial choices
// (fixes that don't apply are switched off) and receives the final ones
// (also for "Sun check", so they come back when the window opens again).
FixWindowResult ShowFixWindow(HWND owner, const std::wstring& fileName, const HdriReport& report, HdriFixOptions& opts);
