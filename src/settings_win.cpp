// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace {

constexpr wchar_t kView[] = L"View";
constexpr wchar_t kExport[] = L"Export";
constexpr wchar_t kWindow[] = L"Window";

std::wstring ExeFolder() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return p.substr(0, slash + 1);
}

std::wstring IniPath() {
    const std::wstring f = ExeFolder();
    return f.empty() ? f : f + L"EXRay.ini";
}

// Before v0.9.2 the app was called "HDR & EXR file preview".
std::wstring LegacyIniPath() {
    const std::wstring f = ExeFolder();
    return f.empty() ? f : f + L"HDR-EXR-Preview.ini";
}

std::wstring Get(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[4096];
    GetPrivateProfileStringW(section, key, L"", buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    return buf;
}

float GetFloat(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float def) {
    std::wstring v = Get(ini, section, key);
    if (v.empty()) return def;
    wchar_t* end = nullptr;
    float f = std::wcstof(v.c_str(), &end);
    return (end && end != v.c_str()) ? f : def;
}

int GetInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, ini.c_str()));
}

bool Put(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str()) != FALSE;
}

std::wstring Num(double v) {
    wchar_t buf[64];
    std::swprintf(buf, std::size(buf), L"%.3f", v);
    return buf;
}

}  // namespace

AppSettings LoadSettings() {
    AppSettings s;
    std::wstring ini = IniPath();
    if (ini.empty()) return s;
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ini = LegacyIniPath();   // settings from before the rename, if any (saved as EXRay.ini from now on)
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return s;
    }

    s.thumbScale = std::clamp(GetFloat(ini, kView, L"ThumbnailSize", 1.0f), 0.5f, 2.0f);
    s.exposureEV = std::clamp(GetFloat(ini, kView, L"Exposure", 0.0f), -10.0f, 10.0f);
    const std::wstring tm = Get(ini, kView, L"Tonemap");
    s.curve = tm == L"Clip" ? Tonemap::Clip : tm == L"Reinhard" ? Tonemap::Reinhard : Tonemap::ACES;

    s.exportFolder = Get(ini, kExport, L"Folder");
    s.exportJpeg = Get(ini, kExport, L"Format") == L"jpg";
    s.exportEveryLayer = GetInt(ini, kExport, L"EveryLayer", 0) != 0;
    const std::wstring look = Get(ini, kExport, L"Look");
    s.exportLook = look == L"aces" ? 1 : look == L"shown" ? 2 : 0;
    const std::wstring fixed = Get(ini, kExport, L"FixedFormat");
    s.fixedFormat = fixed == L"exr16" ? 1 : fixed == L"hdr" ? 2 : 0;

    s.winW = GetInt(ini, kWindow, L"Width", 0);
    s.winH = GetInt(ini, kWindow, L"Height", 0);
    s.winX = GetInt(ini, kWindow, L"X", 0);
    s.winY = GetInt(ini, kWindow, L"Y", 0);
    s.maximized = GetInt(ini, kWindow, L"Maximized", 0) != 0;
    s.hasWindow = s.winW > 200 && s.winH > 150;
    return s;
}

void SaveSettings(const AppSettings& s) {
    const std::wstring ini = IniPath();
    if (ini.empty()) return;

    // Start the file as UTF-16 so folder names in any language survive.
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;   // read-only folder: just don't remember anything
        const unsigned char bom[2] = {0xFF, 0xFE};
        DWORD written = 0;
        WriteFile(h, bom, 2, &written, nullptr);
        CloseHandle(h);
    }

    if (!Put(ini, kView, L"ThumbnailSize", Num(s.thumbScale))) return;
    Put(ini, kView, L"Exposure", Num(s.exposureEV));
    Put(ini, kView, L"Tonemap", s.curve == Tonemap::Clip ? L"Clip" : s.curve == Tonemap::Reinhard ? L"Reinhard" : L"ACES");

    Put(ini, kExport, L"Folder", s.exportFolder);
    Put(ini, kExport, L"Format", s.exportJpeg ? L"jpg" : L"png");
    Put(ini, kExport, L"EveryLayer", s.exportEveryLayer ? L"1" : L"0");
    Put(ini, kExport, L"Look", s.exportLook == 1 ? L"aces" : s.exportLook == 2 ? L"shown" : L"clip");
    Put(ini, kExport, L"FixedFormat", s.fixedFormat == 1 ? L"exr16" : s.fixedFormat == 2 ? L"hdr" : L"exr32");

    if (s.hasWindow) {
        Put(ini, kWindow, L"X", std::to_wstring(s.winX));
        Put(ini, kWindow, L"Y", std::to_wstring(s.winY));
        Put(ini, kWindow, L"Width", std::to_wstring(s.winW));
        Put(ini, kWindow, L"Height", std::to_wstring(s.winH));
        Put(ini, kWindow, L"Maximized", s.maximized ? L"1" : L"0");
    }
}
