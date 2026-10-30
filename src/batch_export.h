// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Exporting many files (or every layer of them) to PNG / JPEG, one at a time.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "export.h"
#include "image_io.h"

struct BatchJob {
    std::vector<PathString> files;
    PathString outFolder;
    ExportFormat format = ExportFormat::Png;
    bool everyLayer = false;          // multi-layer EXRs: one file per layer
    TonemapSettings look{0.0f, Tonemap::Clip};   // exposure + tonemap for colour images
};

struct BatchResult {
    int exported = 0;                 // files written
    int processed = 0;                // source files finished (ok or not)
    bool cancelled = false;
    std::vector<std::pair<PathString, std::string>> failures;   // source file, reason
    PathString lastWritten;
};

// Called after each source file: (finished so far, total, file being worked on next).
using BatchProgress = std::function<void(int done, int total, const PathString& current)>;

// Runs the whole batch on the calling thread. Colour images get `job.look`;
// Cryptomatte and data passes (depth, normals...) are saved as their views.
// Existing files are never overwritten: "name (2).png" and so on.
BatchResult RunBatchExport(const BatchJob& job, const std::atomic<bool>& cancel, const BatchProgress& progress);

// "folder/stem.ext", or "folder/stem (2).ext" etc. if taken (on disk or in `reserved`).
PathString UniqueOutputPath(const PathString& folder, const PathString& stem, const PathString& ext,
                            std::vector<PathString>& reserved);
