// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_export.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>

#include "exr_doc.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32
const PathString kSlash = L"\\";
PathString Lit(const char* s) { PathString r; while (*s) r += static_cast<wchar_t>(*s++); return r; }
PathString FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    PathString w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
bool Exists(const PathString& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool SameName(const PathString& a, const PathString& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
#else
const PathString kSlash = "/";
PathString Lit(const char* s) { return s; }
PathString FromUtf8(const std::string& s) { return s; }
bool Exists(const PathString& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
bool SameName(const PathString& a, const PathString& b) { return a == b; }
#endif

PathString FileStem(const PathString& path) {
    size_t slash = path.find_last_of(Lit("\\/"));
    PathString name = slash == PathString::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == PathString::npos ? name : name.substr(0, dot);
}

// Layer names can contain '/' or other characters not allowed in file names.
PathString SafePart(const std::string& utf8) {
    PathString s = FromUtf8(utf8);
    for (auto& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return s;
}

}  // namespace

PathString UniqueOutputPath(const PathString& folder, const PathString& stem, const PathString& ext,
                            std::vector<PathString>& reserved) {
    PathString base = folder;
    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += kSlash;
    auto taken = [&](const PathString& p) {
        if (Exists(p)) return true;
        return std::any_of(reserved.begin(), reserved.end(), [&](const PathString& r) { return SameName(r, p); });
    };
    PathString candidate = base + stem + ext;
    for (int n = 2; taken(candidate) && n < 100000; ++n) {
        char num[24];
        std::snprintf(num, sizeof num, " (%d)", n);
        candidate = base + stem + Lit(num) + ext;
    }
    reserved.push_back(candidate);
    return candidate;
}

BatchResult RunBatchExport(const BatchJob& job, const std::atomic<bool>& cancel, const BatchProgress& progress) {
    BatchResult res;
    const PathString ext = job.format == ExportFormat::Jpeg ? Lit(".jpg") : Lit(".png");
    const int total = static_cast<int>(job.files.size());
    std::vector<PathString> reserved;

    for (int i = 0; i < total; ++i) {
        if (cancel.load()) { res.cancelled = true; break; }
        const PathString& src = job.files[static_cast<size_t>(i)];
        if (progress) progress(i, total, src);
        std::string error;
        try {
            std::vector<unsigned char> data;
            HdrImage img;
            std::shared_ptr<ExrDocument> doc;
            bool ok = ReadFileBytes(src, data, error) && DecodeHdrImage(data, img, error, job.everyLayer ? &doc : nullptr);
            std::vector<unsigned char>().swap(data);   // free the file bytes early
            if (ok) {
                const PathString stem = FileStem(src);
                if (job.everyLayer && doc) {
                    img.clear();   // rebuilt per layer below; keep only one in memory
                    for (int l = 0; l < static_cast<int>(doc->Layers().size()) && !cancel.load(); ++l) {
                        HdrImage li;
                        std::string lerr;
                        if (!doc->BuildLayer(l, li, lerr)) { res.failures.push_back({src, lerr}); continue; }
                        // The unnamed main image keeps the plain name; other layers get a suffix.
                        const std::string& lname = doc->Layers()[static_cast<size_t>(l)].name;
                        const PathString out = UniqueOutputPath(job.outFolder,
                            lname.empty() ? stem : stem + Lit("_") + SafePart(lname), ext, reserved);
                        if (ExportImage(li, job.look, out, job.format, lerr)) { ++res.exported; res.lastWritten = out; }
                        else res.failures.push_back({src, lerr});
                    }
                } else {
                    const PathString out = UniqueOutputPath(job.outFolder, stem, ext, reserved);
                    if (ExportImage(img, job.look, out, job.format, error)) { ++res.exported; res.lastWritten = out; }
                    else res.failures.push_back({src, error});
                }
            } else {
                res.failures.push_back({src, error});
            }
        } catch (const std::bad_alloc&) {
            res.failures.push_back({src, "Not enough memory for this image."});
        }
        res.processed = i + 1;
    }
    if (progress) progress(res.processed, total, PathString());
    return res;
}
