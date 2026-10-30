// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Tests the folder library: adding / removing / undo, saving, scanning a
// folder tree, the sidebar rows and searching. Builds a small tree of empty
// .hdr / .exr files in a temporary folder.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "library.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void Touch(const fs::path& p) { fs::create_directories(p.parent_path()); std::ofstream(p) << "x"; }

int main() {
    // --- paths
    CHECK(NormalizeFolderPath(L"D:\\HDRIs\\") == L"D:\\HDRIs");
    CHECK(NormalizeFolderPath(L"D:\\") == L"D:");
    CHECK(NormalizeFolderPath(L"/") == L"/");
    CHECK(SamePathNoCase(L"D:\\Hdris\\Sky", L"d:/HDRIS/sky"));
    CHECK(IsInsideFolder(L"D:\\HDRIs\\Poly Haven\\a.hdr", L"D:\\HDRIs"));
    CHECK(IsInsideFolder(L"D:\\HDRIs", L"D:\\HDRIs\\"));
    CHECK(!IsInsideFolder(L"D:\\HDRIs2\\a.hdr", L"D:\\HDRIs"));
    CHECK(IsInsideFolder(L"D:\\x\\a.hdr", L"D:"));
    CHECK(LeafName(L"D:\\HDRIs\\Poly Haven") == L"Poly Haven");
    CHECK(LeafName(L"D:") == L"D:");
    CHECK(NaturalLess(L"sky2.hdr", L"sky10.hdr"));
    CHECK(!NaturalLess(L"sky10.hdr", L"sky2.hdr"));
    CHECK(NaturalLess(L"Alpha", L"beta"));
    CHECK(NaturalLess(L"sky_02", L"sky_3"));

    // --- add / remove / undo / save
    FolderLibrary lib;
    CHECK(lib.Add(L"D:\\HDRIs\\") == 0);
    CHECK(lib.Add(L"E:\\Renders") == 1);
    CHECK(lib.Add(L"d:\\hdris") == 0);          // already there
    CHECK(lib.Size() == 2);
    CHECK(lib.Add(L"") == -1);
    lib.SetSubfolders(1, true);
    CHECK(lib.RootOf(L"D:\\HDRIs\\Poly Haven\\x.hdr") == 0);
    CHECK(lib.RootOf(L"C:\\x.hdr") == -1);
    lib.Add(L"D:\\HDRIs\\Poly Haven");
    CHECK(lib.RootOf(L"D:\\HDRIs\\Poly Haven\\x.hdr") == 2);   // the deepest one
    CHECK(lib.Remove(0));
    CHECK(lib.Size() == 2 && lib.Folders()[0].path == L"E:\\Renders");
    CHECK(lib.CanUndo() && lib.UndoPath() == L"D:\\HDRIs");
    CHECK(lib.Undo());
    CHECK(lib.Size() == 3 && lib.Folders()[0].path == L"D:\\HDRIs");
    CHECK(!lib.CanUndo());
    const std::vector<std::wstring> saved = lib.Save();
    CHECK(saved.size() == 3 && saved[1] == L"1|E:\\Renders");
    FolderLibrary lib2;
    lib2.Load(saved);
    CHECK(lib2.Save() == saved);
    lib2.Load({L"bad", L"0|", L"1|F:\\X", L"0|f:\\x"});
    CHECK(lib2.Size() == 1 && lib2.Folders()[0].subfolders);

    // --- scanning a real folder tree
    const fs::path tmp = fs::temp_directory_path() / "exray_library_test";
    fs::remove_all(tmp);
    Touch(tmp / "Poly Haven" / "sky10.hdr");
    Touch(tmp / "Poly Haven" / "sky2.HDR");
    Touch(tmp / "Poly Haven" / "notes.txt");
    Touch(tmp / "Poly Haven" / "Night" / "river_night.exr");
    Touch(tmp / "Studio" / "softbox.exr");
    Touch(tmp / ".hidden" / "secret.hdr");
    Touch(tmp / "top_river.hdr");
    fs::create_directories(tmp / "Empty");

    const std::wstring root = tmp.wstring();
    ScanResult scan = ScanFolderTree(root);
    CHECK(!scan.missing);
    CHECK(scan.folders.size() == 5);                        // root, Empty, Poly Haven, Studio, Night
    CHECK(scan.folders[0].total == 5);                      // hidden folder skipped
    CHECK(scan.folders[0].files.size() == 1);
    const int ph = scan.Find(root + L"/Poly Haven");
    CHECK(ph >= 0);
    if (ph >= 0) {
        CHECK(scan.folders[ph].files.size() == 2);
        CHECK(scan.folders[ph].files[0] == L"sky2.HDR");   // natural order, case kept
        CHECK(scan.folders[ph].total == 3);
        CHECK(scan.folders[ph].children.size() == 1);
    }
    CHECK(scan.Find(root + L"/.hidden") < 0);

    ScanResult gone = ScanFolderTree(root + L"/does_not_exist");
    CHECK(gone.missing && gone.folders.empty());

    ScanResult small = ScanFolderTree(root, 2);
    CHECK(small.truncated && small.folders.size() == 2);
    ScanResult shallow = ScanFolderTree(root, 5000, 0);
    CHECK(shallow.truncated && shallow.folders.size() == 1 && shallow.folders[0].total == 1);

    // --- files of a folder
    const auto direct = FilesFromScan(scan, root + L"/Poly Haven", false);
    CHECK(direct.size() == 2);
    const auto withSub = FilesFromScan(scan, root + L"/Poly Haven", true);
    CHECK(withSub.size() == 3 && withSub[2].find(L"river_night.exr") != std::wstring::npos);
    CHECK(FilesFromScan(scan, root, true).size() == 5);

    // --- tree rows
    FolderLibrary tl;
    tl.Add(root);
    tl.Add(root + L"/does_not_exist");
    tl.Add(L"/also/not/scanned/yet");
    std::set<std::wstring> open = {root};
    auto isOpen = [&](const std::wstring& p) { return open.count(NormalizeFolderPath(p)) > 0; };
    std::vector<const ScanResult*> scans = {&scan, &gone, nullptr};
    auto rows = BuildTreeRows(tl, scans, isOpen);
    // root, Empty, Poly Haven, Studio (Night hidden: Poly Haven closed), missing, unscanned
    CHECK(rows.size() == 6);
    if (rows.size() == 6) {
        CHECK(rows[0].depth == 0 && rows[0].expanded && rows[0].count == 5 && rows[0].hasChildren);
        CHECK(rows[1].name == L"Empty" && rows[1].count == 0 && !rows[1].hasChildren);
        CHECK(rows[2].name == L"Poly Haven" && rows[2].hasChildren && !rows[2].expanded && rows[2].count == 3);
        CHECK(rows[4].missing && rows[4].root == 1);
        CHECK(!rows[5].scanned && !rows[5].missing && rows[5].root == 2);
    }
    open.insert(root + L"/Poly Haven");
    rows = BuildTreeRows(tl, scans, isOpen);
    CHECK(rows.size() == 7 && rows[3].name == L"Night" && rows[3].depth == 2);

    // --- search
    std::vector<const ScanResult*> all = {&scan};
    auto hits = SearchLibrary(all, L"river");
    CHECK(hits.size() == 2);
    hits = SearchLibrary(all, L"RIVER night");
    CHECK(hits.size() == 1 && hits[0].path.find(L"river_night.exr") != std::wstring::npos);
    CHECK(SearchLibrary(all, L"   ").empty());
    CHECK(SearchLibrary(all, L"sky").size() == 2);
    CHECK(SearchLibrary(all, L"river", root + L"/Studio").empty());
    CHECK(SearchLibrary(all, L"river", root + L"/Poly Haven").size() == 1);
    CHECK(SearchLibrary(all, L"notes").empty());               // only .hdr / .exr files
    std::vector<const ScanResult*> twice = {&scan, &scan};     // nested library folders: no duplicates
    CHECK(SearchLibrary(twice, L"sky").size() == 2);
    CHECK(SearchLibrary(all, L"s", L"", 1).size() == 1);

    // Nothing in the test folder was changed or removed by any of this.
    CHECK(fs::exists(tmp / "Poly Haven" / "sky10.hdr") && fs::exists(tmp / "Poly Haven" / "notes.txt"));
    fs::remove_all(tmp);

    std::printf(failures ? "FAILED (%d)\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
