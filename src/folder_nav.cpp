// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "folder_nav.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include "image_io.h"

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FolderOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::vector<FileEntry> ListHdrFileEntries(const std::wstring& folder) {
    std::vector<FileEntry> entries;
    if (folder.empty()) return entries;

    WIN32_FIND_DATAW fd;
    std::wstring pattern = folder + L"\\*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!IsSupportedExtension(std::wstring(fd.cFileName))) continue;
        FileEntry e;
        e.path = fd.cFileName;   // name for now; made a full path after sorting
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.writeTime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return StrCmpLogicalW(a.path.c_str(), b.path.c_str()) < 0;
    });
    for (auto& e : entries) e.path = folder + L"\\" + e.path;
    return entries;
}

std::vector<std::wstring> ListHdrFilesInFolder(const std::wstring& folder) {
    std::vector<std::wstring> paths;
    for (auto& e : ListHdrFileEntries(folder)) paths.push_back(std::move(e.path));
    return paths;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

int FindFileIndex(const std::vector<std::wstring>& files, const std::wstring& path, bool& exact) {
    const std::wstring name = FileNameOf(path);
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring other = FileNameOf(files[i]);
        if (CompareStringOrdinal(name.c_str(), -1, other.c_str(), -1, TRUE) == CSTR_EQUAL) {
            exact = true;
            return static_cast<int>(i);
        }
    }
    exact = false;
    int i = 0;
    while (i < static_cast<int>(files.size()) &&
           StrCmpLogicalW(FileNameOf(files[i]).c_str(), name.c_str()) < 0)
        ++i;
    return i;
}
