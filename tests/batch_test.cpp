// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Usage: batch_test <out_folder> <every_layer 0|1> <png|jpg> <files...>
#include <chrono>
#include <cstdio>
#include "batch_export.h"
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    BatchJob job;
    job.outFolder = argv[1];
    job.everyLayer = argv[2][0] == '1';
    job.format = std::string(argv[3]) == "jpg" ? ExportFormat::Jpeg : ExportFormat::Png;
    for (int i = 4; i < argc; ++i) job.files.push_back(argv[i]);
    std::atomic<bool> cancel{false};
    auto t = std::chrono::steady_clock::now();
    BatchResult r = RunBatchExport(job, cancel, [](int d, int n, const std::string& f) { std::printf("  %d/%d %s\n", d, n, f.c_str()); });
    std::printf("exported %d, processed %d, failures %zu, %.1fs\n", r.exported, r.processed, r.failures.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    for (auto& f : r.failures) std::printf("  FAIL %s: %s\n", f.first.c_str(), f.second.c_str());
    return 0;
}
