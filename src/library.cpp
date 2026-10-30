// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <system_error>


namespace fs = std::filesystem;

namespace {

bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

std::wstring Join(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty()) return name;
    if (IsSep(folder.back())) return folder + name;
    return folder + kSep + name;
}

// The path to hand to the file system: "D:" alone would mean "the current
// folder on drive D", so a drive gets its separator back.
fs::path FsPath(const std::wstring& folder) {
    if (folder.size() == 2 && folder[1] == L':') return fs::path(folder + L'\\');
    return fs::path(folder);
}

wchar_t Lower(wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))); }

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = Lower(c);
    return s;
}

// The files EXRay shows: .hdr and .exr (any case).
bool IsHdrName(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = ToLower(name.substr(dot + 1));
    return ext == L"hdr" || ext == L"exr";
}

bool SkipFolderName(const std::wstring& name) {
    if (name.empty() || name[0] == L'.' || name[0] == L'$') return true;   // .git, $RECYCLE.BIN ...
    return SamePathNoCase(name, L"System Volume Information");
}

}  // namespace

std::wstring NormalizeFolderPath(std::wstring path) {
    while (path.size() > 1 && IsSep(path.back())) path.pop_back();
    return path;
}

bool SamePathNoCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const wchar_t x = a[i], y = b[i];
        if (IsSep(x) && IsSep(y)) continue;
        if (Lower(x) != Lower(y)) return false;
    }
    return true;
}

bool IsInsideFolder(const std::wstring& path, const std::wstring& folderIn) {
    const std::wstring folder = NormalizeFolderPath(folderIn);
    if (folder.empty()) return false;
    if (SamePathNoCase(NormalizeFolderPath(path), folder)) return true;
    if (path.size() <= folder.size()) return false;
    if (!SamePathNoCase(path.substr(0, folder.size()), folder)) return false;
    return IsSep(folder.back()) || IsSep(path[folder.size()]);
}

std::wstring LeafName(const std::wstring& pathIn) {
    const std::wstring path = NormalizeFolderPath(pathIn);
    size_t i = path.size();
    while (i > 0 && !IsSep(path[i - 1])) --i;
    std::wstring leaf = path.substr(i);
    return leaf.empty() ? path : leaf;
}

bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::iswdigit(a[i]) && std::iswdigit(b[j])) {
            size_t i0 = i, j0 = j;
            while (i0 < a.size() && a[i0] == L'0') ++i0;   // leading zeros don't count
            while (j0 < b.size() && b[j0] == L'0') ++j0;
            size_t i1 = i0, j1 = j0;
            while (i1 < a.size() && std::iswdigit(a[i1])) ++i1;
            while (j1 < b.size() && std::iswdigit(b[j1])) ++j1;
            if (i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0;          // more digits = bigger
            const int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
            if (c != 0) return c < 0;
            i = i1;
            j = j1;
            continue;
        }
        const wchar_t x = Lower(a[i]), y = Lower(b[j]);
        if (x != y) return x < y;
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;   // same apart from case: a stable order
}

// --- the library -----------------------------------------------------------------------

int FolderLibrary::Add(const std::wstring& pathIn) {
    const std::wstring path = NormalizeFolderPath(pathIn);
    if (path.empty()) return -1;
    if (int i = IndexOf(path); i >= 0) return i;
    folders_.push_back({path, false});
    return static_cast<int>(folders_.size()) - 1;
}

bool FolderLibrary::Remove(size_t index) {
    if (index >= folders_.size()) return false;
    undo_ = folders_[index];
    undoIndex_ = index;
    hasUndo_ = true;
    folders_.erase(folders_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool FolderLibrary::Undo() {
    if (!hasUndo_) return false;
    hasUndo_ = false;
    if (IndexOf(undo_.path) >= 0) return false;   // added again meanwhile
    const size_t at = std::min(undoIndex_, folders_.size());
    folders_.insert(folders_.begin() + static_cast<std::ptrdiff_t>(at), undo_);
    return true;
}

void FolderLibrary::SetSubfolders(size_t index, bool on) {
    if (index < folders_.size()) folders_[index].subfolders = on;
}

int FolderLibrary::IndexOf(const std::wstring& path) const {
    const std::wstring p = NormalizeFolderPath(path);
    for (size_t i = 0; i < folders_.size(); ++i)
        if (SamePathNoCase(folders_[i].path, p)) return static_cast<int>(i);
    return -1;
}

int FolderLibrary::RootOf(const std::wstring& path) const {
    int best = -1;
    size_t bestLen = 0;
    for (size_t i = 0; i < folders_.size(); ++i)
        if (IsInsideFolder(path, folders_[i].path) && folders_[i].path.size() >= bestLen) {
            best = static_cast<int>(i);
            bestLen = folders_[i].path.size();
        }
    return best;
}

std::vector<std::wstring> FolderLibrary::Save() const {
    std::vector<std::wstring> lines;
    lines.reserve(folders_.size());
    for (const auto& f : folders_) lines.push_back((f.subfolders ? L"1|" : L"0|") + f.path);
    return lines;
}

void FolderLibrary::Load(const std::vector<std::wstring>& lines) {
    folders_.clear();
    hasUndo_ = false;
    for (const auto& line : lines) {
        if (line.size() < 3 || line[1] != L'|') continue;
        const size_t before = folders_.size();
        const int i = Add(line.substr(2));
        if (i >= 0 && folders_.size() > before) folders_[static_cast<size_t>(i)].subfolders = line[0] == L'1';
    }
}

// --- scanning -----------------------------------------------------------------------------

int ScanResult::Find(const std::wstring& path) const {
    for (size_t i = 0; i < folders.size(); ++i)
        if (SamePathNoCase(folders[i].path, NormalizeFolderPath(path))) return static_cast<int>(i);
    return -1;
}

ScanResult ScanFolderTree(const std::wstring& rootIn, int maxFolders, int maxDepth) {
    ScanResult res;
    res.root = NormalizeFolderPath(rootIn);
    std::error_code ec;
    if (res.root.empty() || !fs::is_directory(FsPath(res.root), ec)) {
        res.missing = true;
        return res;
    }
    try {
        res.folders.push_back(ScannedFolder{res.root, -1, 0, {}, {}, 0});
        for (size_t at = 0; at < res.folders.size(); ++at) {
            std::vector<std::wstring> subNames;
            std::vector<std::wstring> files;
            const std::wstring here = res.folders[at].path;
            const int depth = res.folders[at].depth;
            fs::directory_iterator it(FsPath(here), fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;   // unreadable: shown empty
            for (const fs::directory_iterator end; it != end; it.increment(ec)) {
                if (ec) break;
                const fs::directory_entry& e = *it;
                const std::wstring name = e.path().filename().wstring();
                std::error_code ec2;
                if (e.is_symlink(ec2)) continue;   // links and junctions could loop
                if (e.is_directory(ec2)) {
                    if (!SkipFolderName(name)) subNames.push_back(name);
                } else if (e.is_regular_file(ec2) && IsHdrName(name)) {
                    files.push_back(name);
                }
            }
            std::sort(files.begin(), files.end(), NaturalLess);
            std::sort(subNames.begin(), subNames.end(), NaturalLess);
            res.folders[at].files = std::move(files);
            if (depth >= maxDepth) {
                if (!subNames.empty()) res.truncated = true;
                continue;
            }
            for (const auto& n : subNames) {
                if (static_cast<int>(res.folders.size()) >= maxFolders) { res.truncated = true; break; }
                res.folders[at].children.push_back(static_cast<int>(res.folders.size()));
                res.folders.push_back(ScannedFolder{Join(here, n), static_cast<int>(at), depth + 1, {}, {}, 0});
            }
        }
    } catch (...) {
        // A path the system can't convert, out of memory...: keep what was found.
    }
    // Totals bottom-up: children always come after their parent.
    for (size_t k = res.folders.size(); k-- > 0;) {
        ScannedFolder& f = res.folders[k];
        f.total = static_cast<int>(f.files.size());
        for (int c : f.children) f.total += res.folders[static_cast<size_t>(c)].total;
    }
    return res;
}

// --- the tree -----------------------------------------------------------------------------

namespace {

void AddRows(const ScanResult& scan, int folder, int root, int depth,
             const std::function<bool(const std::wstring&)>& isExpanded, std::vector<TreeRow>& rows) {
    const ScannedFolder& f = scan.folders[static_cast<size_t>(folder)];
    TreeRow r;
    r.path = f.path;
    r.name = depth == 0 ? f.path : LeafName(f.path);
    r.root = root;
    r.depth = depth;
    r.hasChildren = !f.children.empty();
    r.expanded = r.hasChildren && isExpanded(f.path);
    r.scanned = true;
    r.count = f.total;
    rows.push_back(r);
    if (r.expanded)
        for (int c : f.children) AddRows(scan, c, root, depth + 1, isExpanded, rows);
}

}  // namespace

std::vector<TreeRow> BuildTreeRows(const FolderLibrary& lib, const std::vector<const ScanResult*>& scans,
                                   const std::function<bool(const std::wstring&)>& isExpanded) {
    std::vector<TreeRow> rows;
    for (size_t i = 0; i < lib.Size(); ++i) {
        const LibraryFolder& lf = lib.Folders()[i];
        const ScanResult* s = i < scans.size() ? scans[i] : nullptr;
        if (s && !s->missing && !s->folders.empty()) {
            AddRows(*s, 0, static_cast<int>(i), 0, isExpanded, rows);
            continue;
        }
        TreeRow r;
        r.path = lf.path;
        r.name = lf.path;
        r.root = static_cast<int>(i);
        r.missing = s && s->missing;
        rows.push_back(r);
    }
    return rows;
}

// --- search -----------------------------------------------------------------------------

std::vector<SearchHit> SearchLibrary(const std::vector<const ScanResult*>& scans, const std::wstring& query,
                                     const std::wstring& within, size_t maxHits) {
    std::vector<std::wstring> words;
    {
        std::wstring w;
        for (wchar_t c : query) {
            if (std::iswspace(c)) { if (!w.empty()) words.push_back(ToLower(w)); w.clear(); }
            else w += c;
        }
        if (!w.empty()) words.push_back(ToLower(w));
    }
    std::vector<SearchHit> hits;
    if (words.empty()) return hits;

    struct Found { std::wstring folder, name; int root; };
    std::vector<Found> found;
    for (size_t r = 0; r < scans.size(); ++r) {
        const ScanResult* s = scans[r];
        if (!s || s->missing) continue;
        for (const ScannedFolder& f : s->folders) {
            if (!within.empty() && !IsInsideFolder(f.path, within)) continue;
            for (const auto& name : f.files) {
                const std::wstring low = ToLower(name);
                bool all = true;
                for (const auto& w : words)
                    if (low.find(w) == std::wstring::npos) { all = false; break; }
                if (all) found.push_back({f.path, name, static_cast<int>(r)});
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (!SamePathNoCase(a.folder, b.folder)) return NaturalLess(a.folder, b.folder);
        return NaturalLess(a.name, b.name);
    });
    // The same folder can be in two library folders (one inside the other): list each file once.
    for (const auto& f : found) {
        std::wstring path = Join(f.folder, f.name);
        if (!hits.empty() && SamePathNoCase(hits.back().path, path)) continue;
        hits.push_back({std::move(path), f.root});
        if (hits.size() >= maxHits) break;
    }
    return hits;
}

std::vector<std::wstring> FilesFromScan(const ScanResult& scan, const std::wstring& folder, bool subfolders) {
    std::vector<std::wstring> out;
    const int start = scan.Find(folder);
    if (start < 0) return out;
    std::vector<int> stack{start};
    while (!stack.empty()) {
        const int k = stack.back();
        stack.pop_back();
        const ScannedFolder& f = scan.folders[static_cast<size_t>(k)];
        for (const auto& name : f.files) out.push_back(Join(f.path, name));
        if (!subfolders) break;
        for (auto c = f.children.rbegin(); c != f.children.rend(); ++c) stack.push_back(*c);
    }
    return out;
}
