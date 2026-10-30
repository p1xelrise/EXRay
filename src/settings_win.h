// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings kept in EXRay.ini next to the .exe (so the app stays
// portable). If that folder can't be written, nothing is saved, silently.
#pragma once

#include <string>
#include <vector>
#include "tonemap.h"

struct AppSettings {
    float thumbScale = 1.0f;          // grid thumbnail size, 0.5 .. 2.0
    float exposureEV = 0.0f;
    Tonemap curve = Tonemap::ACES;

    std::wstring exportFolder;
    bool exportJpeg = false;
    int exportLook = 0;               // 0 = Clip (raw sRGB), 1 = ACES, 2 = as shown on screen
    bool exportEveryLayer = false;

    int fixedFormat = 0;              // Fix HDRI save: 0 = EXR 32-bit, 1 = EXR 16-bit half, 2 = Radiance .hdr

    // The folder library (sidebar): one entry per folder, "1|D:\HDRIs" (1 = with subfolders).
    std::vector<std::wstring> libraryFolders;
    std::vector<std::wstring> expandedFolders;   // folders showing their subfolders in the tree
    std::wstring lastFolder;                     // opened again at start when nothing else is
    bool showSidebar = true;
    bool showInspector = true;

    bool hasWindow = false;           // the next four are valid
    int winX = 0, winY = 0, winW = 0, winH = 0;
    bool maximized = false;
};

AppSettings LoadSettings();
void SaveSettings(const AppSettings& s);
