// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Usage: export_test <in> <out.png|out.jpg>
#include <chrono>
#include <cstdio>
#include "export.h"
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HdrImage img; std::string err;
    if (!LoadHdrFile(argv[1], img, err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    auto t = std::chrono::steady_clock::now();
    bool ok = ExportImage(img, TonemapSettings{}, argv[2], ExportFormatFromPath(argv[2]), err);
    std::printf("%s %dx%d -> %s in %.2fs %s\n", ok ? "ok" : "FAILED", img.width, img.height, argv[2],
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(), err.c_str());
    return ok ? 0 : 1;
}
