// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "thumb_pool.h"

#include <algorithm>
#include <climits>
#include <new>

ThumbPool::ThumbPool(int workers, Deliver deliver) : deliver_(std::move(deliver)) {
    for (int i = 0; i < std::max(1, workers); ++i) threads_.emplace_back([this] { WorkerLoop(); });
}

ThumbPool::~ThumbPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void ThumbPool::SetJobs(unsigned generation, std::vector<PathString> paths, int boxW, int boxH,
                        const std::vector<unsigned char>& alreadyDone) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        generation_ = generation;
        paths_ = std::move(paths);
        state_.assign(paths_.size(), 0);
        big_.assign(paths_.size(), 0);
        remaining_ = static_cast<int>(paths_.size());
        if (alreadyDone.size() == paths_.size())
            for (size_t i = 0; i < paths_.size(); ++i)
                if (alreadyDone[i]) { state_[i] = 2; --remaining_; }
        boxW_ = boxW;
        boxH_ = boxH;
        focusFirst_ = 0;
        focusLast_ = -1;
    }
    cv_.notify_all();
}

void ThumbPool::Requeue(unsigned generation, int index) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != generation_ || index < 0 || index >= static_cast<int>(state_.size())) return;
        if (state_[static_cast<size_t>(index)] == 2) ++remaining_;
        state_[static_cast<size_t>(index)] = 0;
    }
    cv_.notify_all();
}

void ThumbPool::SetFocus(int first, int last) {
    std::lock_guard<std::mutex> lock(mu_);
    focusFirst_ = first;
    focusLast_ = last;
}

void ThumbPool::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        paused_ = paused;
    }
    cv_.notify_all();
}

int ThumbPool::Remaining() const {
    std::lock_guard<std::mutex> lock(mu_);
    return remaining_;
}

int ThumbPool::PickJobLocked() const {
    int best = -1;
    long long bestScore = LLONG_MAX;
    for (int i = 0; i < static_cast<int>(state_.size()); ++i) {
        if (state_[static_cast<size_t>(i)] != 0) continue;
        if (bigBusy_ && big_[static_cast<size_t>(i)]) continue;   // wait for the big slot
        long long score;
        if (i < focusFirst_) score = (focusFirst_ - i) * 3LL;          // above the screen: a bit later
        else if (i > focusLast_) score = (i - focusLast_) * 2LL;       // below: scrolling usually goes down
        else score = 0;
        score = score * 100000 + i;                                   // ties: top to bottom
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

void ThumbPool::WorkerLoop() {
    for (;;) {
        unsigned gen;
        int index;
        PathString path;
        int boxW, boxH;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || (!paused_ && PickJobLocked() >= 0); });
            if (stop_) return;
            index = PickJobLocked();
            state_[static_cast<size_t>(index)] = 1;
            gen = generation_;
            path = paths_[static_cast<size_t>(index)];
            boxW = boxW_;
            boxH = boxH_;
        }

        auto res = std::make_unique<ThumbResult>();
        res->generation = gen;
        res->index = index;
        // Releases the "big decode" slot however the block below is left.
        struct BigSlot {
            ThumbPool* pool = nullptr;
            ~BigSlot() {
                if (!pool) return;
                { std::lock_guard<std::mutex> lock(pool->mu_); pool->bigBusy_ = false; }
                pool->cv_.notify_all();
            }
        } bigSlot;

        try {
            std::vector<unsigned char> data;
            if (ReadFileBytes(path, data, res->error)) {
                int w = 0, h = 0;
                PeekImageSize(data, w, h);
                const bool isBig = static_cast<long long>(w) * h > kBigPixels;

                bool stale, postpone = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    stale = (gen != generation_) || stop_;
                    if (!stale && isBig) {
                        big_[static_cast<size_t>(index)] = 1;
                        if (bigBusy_) {
                            // Another huge image is being decoded: put this one back
                            // and do small ones meanwhile instead of waiting.
                            state_[static_cast<size_t>(index)] = 0;
                            postpone = true;
                        } else {
                            bigBusy_ = true;
                            bigSlot.pool = this;
                        }
                    }
                }
                if (postpone) continue;
                if (!stale) {
                    HdrImage img;
                    // .hdr files are shrunk while decoding (to 2048+ px wide: plenty for a thumbnail).
                    if (DecodeHdrImageReduced(data, img, res->error, 2048)) {
                        std::vector<unsigned char>().swap(data);   // free the file bytes early
                        res->imageWidth = w > 0 ? w : img.width;   // the real size, not the reduced one
                        res->imageHeight = h > 0 ? h : img.height;
                        res->format = img.format;
                        res->thumb = MakeThumbnail(img, boxW, boxH);
                        res->ok = true;
                    }
                } else {
                    res->error = "cancelled";
                }
            }
        } catch (const std::bad_alloc&) {
            res->ok = false;
            res->error = "Not enough memory to make a thumbnail of this image.";
        } catch (...) {
            res->ok = false;
            res->error = "Something went wrong while reading this file.";
        }

        bool deliver;
        {
            std::lock_guard<std::mutex> lock(mu_);
            deliver = (gen == generation_) && !stop_;
            if (deliver && state_[static_cast<size_t>(index)] == 1) {
                state_[static_cast<size_t>(index)] = 2;
                --remaining_;
            }
        }
        if (deliver) deliver_(std::move(res));
    }
}
