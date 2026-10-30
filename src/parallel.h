// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Splits a row range across all CPU cores.
#pragma once

#include <algorithm>
#include <thread>
#include <vector>

// Calls fn(y0, y1) on several threads so that together they cover [0, rows).
// Small jobs run on the calling thread only.
template <class Fn>
void ParallelRows(int rows, Fn fn, long long workPerRow = 1 << 16) {
    if (rows <= 0) return;
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    // Don't start threads for tiny jobs (thread start-up costs more than it saves).
    long long total = static_cast<long long>(rows) * workPerRow;
    if (total < (1 << 18)) n = 1;
    n = std::min<unsigned>(n, static_cast<unsigned>(rows));
    if (n == 1) { fn(0, rows); return; }

    const int per = (rows + static_cast<int>(n) - 1) / static_cast<int>(n);
    std::vector<std::thread> workers;
    for (unsigned i = 1; i < n; ++i) {
        int y0 = static_cast<int>(i) * per, y1 = std::min(rows, y0 + per);
        if (y0 < y1) workers.emplace_back([=, &fn] { fn(y0, y1); });
    }
    fn(0, std::min(rows, per));
    for (auto& t : workers) t.join();
}
