// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Native test for the thumbnail workers.
// Usage: thumb_test <out_sheet.png> <files...>
// Makes 312x156 thumbnails with 2 workers, prints timings and failures,
// and writes a contact sheet (tonemapped with default settings).
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

#include "thumb_pool.h"
#include "stb_image_write.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::vector<std::string> files(argv + 2, argv + argc);
    const int n = static_cast<int>(files.size());
    const int boxW = 312, boxH = 156;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::unique_ptr<ThumbResult>> results(static_cast<size_t>(n));
    int done = 0;

    auto t0 = std::chrono::steady_clock::now();
    {
        ThumbPool pool(2, [&](std::unique_ptr<ThumbResult> r) {
            std::lock_guard<std::mutex> lock(mu);
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("[%6.2fs] #%d %s %s\n", t, r->index, r->ok ? "ok  " : "FAIL",
                        r->ok ? "" : r->error.c_str());
            results[static_cast<size_t>(r->index)] = std::move(r);
            ++done;
            cv.notify_all();
        });
        pool.SetJobs(1, files, boxW, boxH);
        pool.SetFocus(n - 4, n - 1);   // pretend the last few are on screen: they should come first
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return done == n; });
        std::printf("remaining reported: %d\n", pool.Remaining());
    }

    // Contact sheet: tiles of 312x156 on a dark background, 6 per row.
    const int cols = 6, rows = (n + cols - 1) / cols, pad = 6;
    const int SW = cols * (boxW + pad) + pad, SH = rows * (boxH + pad) + pad;
    std::vector<uint8_t> sheet(static_cast<size_t>(SW) * SH * 3, 30);
    for (int i = 0; i < n; ++i) {
        const auto& r = results[static_cast<size_t>(i)];
        if (!r || !r->ok) continue;
        std::vector<uint8_t> bgra(static_cast<size_t>(r->thumb.width) * r->thumb.height * 4);
        TonemapThumbnail(r->thumb, TonemapSettings{}, bgra.data());
        const int ox = pad + (i % cols) * (boxW + pad) + (boxW - r->thumb.width) / 2;
        const int oy = pad + (i / cols) * (boxH + pad) + (boxH - r->thumb.height) / 2;
        for (int y = 0; y < r->thumb.height; ++y)
            for (int x = 0; x < r->thumb.width; ++x) {
                const uint8_t* s = &bgra[(static_cast<size_t>(y) * r->thumb.width + x) * 4];
                uint8_t* d = &sheet[(static_cast<size_t>(oy + y) * SW + ox + x) * 3];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            }
    }
    stbi_write_png(argv[1], SW, SH, 3, sheet.data(), SW * 3);
    std::printf("total %.2fs\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
