// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Finding the .hdr / .exr files next to the current one.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FileEntry {
    std::wstring path;
    uint64_t size = 0;
    uint64_t writeTime = 0;   // FILETIME as a number
};

// Like ListHdrFilesInFolder, with size and last-write time for each file.
std::vector<FileEntry> ListHdrFileEntries(const std::wstring& folder);

// Case-insensitive path comparison (Windows file names ignore case).
bool SamePath(const std::wstring& a, const std::wstring& b);

// All .hdr and .exr files in `folder`, sorted the way File Explorer sorts
// names ("sky2" before "sky10").
std::vector<std::wstring> ListHdrFilesInFolder(const std::wstring& folder);

// Index of `path` in `files`, matching by file name without regard to case.
// If it isn't there (e.g. it was deleted), returns where it would be inserted
// and sets `exact` to false.
int FindFileIndex(const std::vector<std::wstring>& files, const std::wstring& path, bool& exact);

std::wstring FileNameOf(const std::wstring& path);
std::wstring FolderOf(const std::wstring& path);
