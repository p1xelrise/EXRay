// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Background workers that make thumbnails for the folder grid.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "image_io.h"
#include "thumbnail.h"

struct ThumbResult {
    unsigned generation = 0;   // which folder listing this belongs to
    int index = -1;            // position in that listing
    bool ok = false;
    std::string error;
    int imageWidth = 0, imageHeight = 0;
    std::string format;
    Thumbnail thumb;
    float peakLum = 0.0f;       // brightest luminance and range in stops (ImageQuickStats)
    float rangeStops = 0.0f;
    bool statsReduced = false;  // measured on a shrunk copy (very large .hdr): the peak is approximate
};

class ThumbPool {
public:
    // `deliver` is called on a worker thread with each finished thumbnail.
    using Deliver = std::function<void(std::unique_ptr<ThumbResult>)>;

    ThumbPool(int workers, Deliver deliver);
    ~ThumbPool();   // stops and waits for the workers

    // Replaces the job list (a new folder). Results for older generations
    // are dropped. Thumbnails fit inside boxW x boxH. Entries marked in
    // `alreadyDone` (same length as paths, or empty) are skipped.
    void SetJobs(unsigned generation, std::vector<PathString> paths, int boxW, int boxH,
                 const std::vector<unsigned char>& alreadyDone = {});

    // Marks one job to be done again (e.g. after the file changed).
    void Requeue(unsigned generation, int index);

    // Items currently on screen; workers do these first, then the nearest others.
    void SetFocus(int first, int last);

    // While paused, workers finish what they're doing but start nothing new
    // (used while a full-size image is loading, to leave it the memory/CPU).
    void SetPaused(bool paused);

    // Number of jobs not yet finished.
    int Remaining() const;

    // Images with more pixels than this are decoded one at a time.
    static constexpr long long kBigPixels = 40'000'000;

private:
    void WorkerLoop();
    int PickJobLocked() const;

    Deliver deliver_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    bool paused_ = false;

    unsigned generation_ = 0;
    std::vector<PathString> paths_;
    std::vector<unsigned char> state_;   // 0 = waiting, 1 = in progress, 2 = done
    int boxW_ = 0, boxH_ = 0;
    int focusFirst_ = 0, focusLast_ = -1;
    int remaining_ = 0;

    std::vector<unsigned char> big_;     // 1 = known to be a very large image
    bool bigBusy_ = false;               // a very large image is being decoded
};
