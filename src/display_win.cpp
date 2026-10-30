// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "display_win.h"

#include <new>
#include "resample.h"

std::unique_ptr<Bitmap> Bitmap::Create(int w, int h) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) throw std::bad_alloc();

    std::unique_ptr<Bitmap> b(new Bitmap());
    b->w_ = w;
    b->h_ = h;
    b->bmp_ = bmp;
    b->bits_ = static_cast<uint8_t*>(bits);
    return b;
}

Bitmap::~Bitmap() {
    if (bmp_) DeleteObject(bmp_);
}

std::unique_ptr<Bitmap> TonemapToBitmap(const HdrImage& img, const TonemapSettings& s) {
    auto b = Bitmap::Create(img.width, img.height);
    TonemapToBGRA8(img, s, b->bits());
    GdiFlush();
    return b;
}

Pyramid BuildPyramid(const HdrImage& img, const TonemapSettings& s) {
    Pyramid levels;
    levels.push_back(TonemapToBitmap(img, s));
    while (levels.back()->width() > 256 && levels.back()->height() > 128) {
        const Bitmap& src = *levels.back();
        auto dst = Bitmap::Create(HalfSize(src.width()), HalfSize(src.height()));
        HalveBGRA8(src.bits(), src.width(), src.height(), dst->bits());
        levels.push_back(std::move(dst));
    }
    GdiFlush();
    return levels;
}
