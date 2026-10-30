// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// The folder library: the folders the user added to the sidebar, a scan of
// each (its subfolders and the .hdr / .exr files in them), the rows of the
// folder tree, and searching file names across the library.
//
// Platform independent (std::filesystem), so it is tested on Linux too.
// Nothing here ever deletes or changes a file: removing a folder only takes
// it off the list.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct LibraryFolder {
    std::wstring path;
    bool subfolders = false;   // the grid shows the files of its subfolders too
};

// "D:\HDRIs\" -> "D:\HDRIs", "D:\" -> "D:" (no trailing separator).
std::wstring NormalizeFolderPath(std::wstring path);

// Case-insensitive comparison, as Windows treats file names.
bool SamePathNoCase(const std::wstring& a, const std::wstring& b);

// True when `path` is `folder` itself or inside it.
bool IsInsideFolder(const std::wstring& path, const std::wstring& folder);

// Last part of a path ("D:\HDRIs\Sky" -> "Sky"); a drive stays "D:".
std::wstring LeafName(const std::wstring& path);

// Sorts like File Explorer: case-insensitive, numbers by value ("sky2" before "sky10").
bool NaturalLess(const std::wstring& a, const std::wstring& b);

class FolderLibrary {
public:
    const std::vector<LibraryFolder>& Folders() const { return folders_; }
    size_t Size() const { return folders_.size(); }

    // Adds a folder at the end. Returns its index; a folder that is already
    // in the library is not added again (its index is returned). -1 for an empty path.
    int Add(const std::wstring& path);

    // Takes a folder off the list (files are never touched). It can be brought back with Undo.
    bool Remove(size_t index);
    bool CanUndo() const { return hasUndo_; }
    std::wstring UndoPath() const { return hasUndo_ ? undo_.path : std::wstring(); }
    bool Undo();

    void SetSubfolders(size_t index, bool on);

    int IndexOf(const std::wstring& path) const;
    // Library folder that contains `path` (the deepest one), or -1.
    int RootOf(const std::wstring& path) const;

    // Saved as one line per folder: "1|D:\HDRIs" (1 = with subfolders). '|' can't be in a Windows path.
    std::vector<std::wstring> Save() const;
    void Load(const std::vector<std::wstring>& lines);

private:
    std::vector<LibraryFolder> folders_;
    bool hasUndo_ = false;
    LibraryFolder undo_;
    size_t undoIndex_ = 0;
};

// One folder found by a scan, with the names of its .hdr / .exr files.
struct ScannedFolder {
    std::wstring path;
    int parent = -1;                  // index in ScanResult::folders, -1 for the root
    int depth = 0;                    // 0 = the library folder itself
    std::vector<std::wstring> files;  // file names only, sorted
    std::vector<int> children;        // subfolders, sorted by name
    int total = 0;                    // files here and in all subfolders
};

struct ScanResult {
    std::wstring root;
    bool missing = false;             // the folder doesn't exist (e.g. an unplugged drive)
    bool truncated = false;           // stopped at the folder limit
    std::vector<ScannedFolder> folders;   // folders[0] is the root (when not missing)

    int Find(const std::wstring& path) const;   // index of a folder, or -1
};

// Walks `root` and its subfolders (hidden and system folders skipped).
// Stops after `maxFolders` folders or `maxDepth` levels. Never throws.
ScanResult ScanFolderTree(const std::wstring& root, int maxFolders = 5000, int maxDepth = 12);

// A row of the folder tree in the sidebar.
struct TreeRow {
    std::wstring path;
    std::wstring name;
    int root = -1;           // index of its library folder
    int depth = 0;           // 0 = a library folder
    bool hasChildren = false;
    bool expanded = false;
    bool missing = false;    // library folder not found
    bool scanned = false;    // counts are known
    int count = 0;           // .hdr / .exr files, subfolders included
};

// Rows for the sidebar. `scans[i]` belongs to library folder i (null while it
// is being scanned). `isExpanded(path)` says whether a folder shows its subfolders.
std::vector<TreeRow> BuildTreeRows(const FolderLibrary& lib, const std::vector<const ScanResult*>& scans,
                                   const std::function<bool(const std::wstring&)>& isExpanded);

struct SearchHit {
    std::wstring path;   // full path of the file
    int root = -1;       // library folder it was found in
};

// Files whose names contain every word of `query` (case-insensitive), in all
// scanned library folders, or only inside `within` when that isn't empty.
// Sorted by folder, then name. At most `maxHits`.
std::vector<SearchHit> SearchLibrary(const std::vector<const ScanResult*>& scans, const std::wstring& query,
                                     const std::wstring& within = std::wstring(), size_t maxHits = 5000);

// Full paths of the files of `folder` (and its subfolders when `subfolders`) from a scan.
std::vector<std::wstring> FilesFromScan(const ScanResult& scan, const std::wstring& folder, bool subfolders);
