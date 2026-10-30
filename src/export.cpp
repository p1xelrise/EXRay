// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "export.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <new>
#include <vector>

#include "parallel.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO          // we open the file ourselves (Unicode paths)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace {

struct Sink {
    FILE* f = nullptr;
    bool failed = false;
};

void WriteToFile(void* ctx, void* data, int size) {
    Sink* s = static_cast<Sink*>(ctx);
    if (s->failed || size <= 0) return;
    if (std::fwrite(data, 1, static_cast<size_t>(size), s->f) != static_cast<size_t>(size)) s->failed = true;
}

}  // namespace

ExportFormat ExportFormatFromPath(const PathString& path) {
    size_t dot = path.find_last_of('.');
    if (dot == PathString::npos) return ExportFormat::Png;
    PathString ext = path.substr(dot + 1);
    std::string e;
    for (auto c : ext) {
        const unsigned u = static_cast<unsigned>(c);
        e += static_cast<char>(u < 128u ? std::tolower(static_cast<int>(u)) : '?');
    }
    return (e == "jpg" || e == "jpeg") ? ExportFormat::Jpeg : ExportFormat::Png;
}

bool ExportImage(const HdrImage& img, const TonemapSettings& s, const PathString& path,
                 ExportFormat format, std::string& error) {
    if (img.empty()) { error = "There is no image to export."; return false; }
    const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);

#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) { error = "The file could not be created. Check that the folder exists and isn't read-only."; return false; }

    std::vector<uint8_t> rgb;
    try {
        std::vector<uint8_t> bgra;
        TonemapToBGRA8(img, s, bgra);
        rgb.resize(n * 3);
        ParallelRows(img.height, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * img.width, e = static_cast<size_t>(y1) * img.width; i < e; ++i) {
                rgb[i * 3 + 0] = bgra[i * 4 + 2];
                rgb[i * 3 + 1] = bgra[i * 4 + 1];
                rgb[i * 3 + 2] = bgra[i * 4 + 0];
            }
        }, img.width);
    } catch (const std::bad_alloc&) {
        error = "Not enough memory to export an image this large.";
        std::fclose(f);
#ifdef _WIN32
        _wremove(path.c_str());
#else
        std::remove(path.c_str());
#endif
        return false;
    }


    Sink sink{f, false};
    int ok;
    if (format == ExportFormat::Jpeg) {
        ok = stbi_write_jpg_to_func(WriteToFile, &sink, img.width, img.height, 3, rgb.data(), 92);
    } else {
        stbi_write_png_compression_level = 6;   // a little faster than the default, nearly as small
        ok = stbi_write_png_to_func(WriteToFile, &sink, img.width, img.height, 3, rgb.data(), img.width * 3);
    }
    const bool closed = std::fclose(f) == 0;
    if (!ok || sink.failed || !closed) {
        error = "Writing the file failed (is the disk full?).";
#ifdef _WIN32
        _wremove(path.c_str());
#else
        std::remove(path.c_str());
#endif
        return false;
    }
    return true;
}

// --- saving linear HDR copies ------------------------------------------------------

#include "tinyexr.h"   // declarations only; the implementation is compiled in image_io.cpp

namespace {

// float -> IEEE half, round to nearest even; values beyond the half range are capped.
uint16_t FloatToHalf(float f) {
    if (!(f == f)) return 0;                       // NaN (already repaired) -> 0
    f = std::clamp(f, -kHalfMax, kHalfMax);
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    x &= 0x7fffffffu;
    if (x < 0x38800000u) {                         // below the smallest normal half: denormal / zero
        if (x < 0x33000000u) return static_cast<uint16_t>(sign);
        const uint32_t m = (x & 0x007fffffu) | 0x00800000u;
        const int shift = 126 - static_cast<int>(x >> 23);   // 14 .. 24
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = ((x - 0x38000000u) >> 13);
    const uint32_t rem = x & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<uint16_t>(sign | std::min<uint32_t>(h, 0x7bffu));
}

void RemoveFile(const PathString& path) {
#ifdef _WIN32
    _wremove(path.c_str());
#else
    std::remove(path.c_str());
#endif
}

bool SaveExr(const HdrImage& img, FILE* f, bool half, std::string& error) {
    const size_t n = static_cast<size_t>(img.width) * img.height;
    const float* px = img.rgb();

    // EXR stores channels in name order: B, G, R; one plane each.
    std::vector<float> planesF;
    std::vector<uint16_t> planesH;
    unsigned char* ptr[3];
    if (half) {
        planesH.resize(n * 3);
        ParallelRows(img.height, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * img.width, e = static_cast<size_t>(y1) * img.width; i < e; ++i)
                for (int c = 0; c < 3; ++c) planesH[static_cast<size_t>(2 - c) * n + i] = FloatToHalf(px[i * 3 + c]);
        }, img.width);
        for (int k = 0; k < 3; ++k) ptr[k] = reinterpret_cast<unsigned char*>(planesH.data() + static_cast<size_t>(k) * n);
    } else {
        planesF.resize(n * 3);
        ParallelRows(img.height, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * img.width, e = static_cast<size_t>(y1) * img.width; i < e; ++i)
                for (int c = 0; c < 3; ++c) planesF[static_cast<size_t>(2 - c) * n + i] = px[i * 3 + c];
        }, img.width);
        for (int k = 0; k < 3; ++k) ptr[k] = reinterpret_cast<unsigned char*>(planesF.data() + static_cast<size_t>(k) * n);
    }

    EXRHeader header;
    InitEXRHeader(&header);
    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = 3;
    image.images = ptr;
    image.width = img.width;
    image.height = img.height;

    EXRChannelInfo channels[3] = {};
    std::strcpy(channels[0].name, "B");
    std::strcpy(channels[1].name, "G");
    std::strcpy(channels[2].name, "R");
    int types[3], requested[3];
    for (int k = 0; k < 3; ++k) {
        types[k] = half ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
        requested[k] = types[k];
    }
    header.num_channels = 3;
    header.channels = channels;
    header.pixel_types = types;
    header.requested_pixel_types = requested;
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    unsigned char* mem = nullptr;
    const char* err = nullptr;
    const size_t size = SaveEXRImageToMemory(&image, &header, &mem, &err);
    if (size == 0 || !mem) {
        error = err ? std::string("The EXR could not be written: ") + err : "The EXR could not be written.";
        if (err) FreeEXRErrorMessage(err);
        if (mem) std::free(mem);
        return false;
    }
    const bool ok = std::fwrite(mem, 1, size, f) == size;
    std::free(mem);
    if (!ok) error = "Writing the file failed (is the disk full?).";
    return ok;
}

// Radiance .hdr: RGBE pixels, run-length encoded scanlines.
void ToRgbe(const float* p, unsigned char out[4]) {
    const float v = std::max({p[0], p[1], p[2]});
    if (!(v > 1e-32f)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e;
    const float scale = std::frexp(v, &e) * 256.0f / v;
    for (int c = 0; c < 3; ++c) out[c] = static_cast<unsigned char>(std::clamp(std::max(0.0f, p[c]) * scale, 0.0f, 255.0f));
    out[3] = static_cast<unsigned char>(std::clamp(e + 128, 0, 255));
}

void RleComponent(const unsigned char* d, int n, std::vector<unsigned char>& out) {
    int i = 0;
    while (i < n) {
        // Find the next run of at least 4 equal bytes.
        int run = i, runLen = 0;
        while (run < n) {
            runLen = 1;
            while (run + runLen < n && runLen < 127 && d[run + runLen] == d[run]) ++runLen;
            if (runLen >= 4) break;
            run += runLen;
        }
        if (run >= n) { run = n; runLen = 0; }
        while (i < run) {   // literal bytes before the run, up to 128 at a time
            const int k = std::min(128, run - i);
            out.push_back(static_cast<unsigned char>(k));
            out.insert(out.end(), d + i, d + i + k);
            i += k;
        }
        if (runLen >= 4) {
            out.push_back(static_cast<unsigned char>(128 + runLen));
            out.push_back(d[run]);
            i = run + runLen;
        }
    }
}

bool SaveRadiance(const HdrImage& img, FILE* f) {
    const int w = img.width, h = img.height;
    char head[160];
    const int hn = std::snprintf(head, sizeof head,
                                 "#?RADIANCE\n# Made with EXRay\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", h, w);
    if (std::fwrite(head, 1, static_cast<size_t>(hn), f) != static_cast<size_t>(hn)) return false;
    const bool rle = w >= 8 && w < 32768;
    std::vector<unsigned char> rgbe(static_cast<size_t>(w) * 4), plane(static_cast<size_t>(w)), line;
    for (int y = 0; y < h; ++y) {
        const float* row = img.rgb() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) ToRgbe(row + x * 3, &rgbe[static_cast<size_t>(x) * 4]);
        line.clear();
        if (!rle) {
            line = rgbe;
        } else {
            line.push_back(2); line.push_back(2);
            line.push_back(static_cast<unsigned char>(w >> 8)); line.push_back(static_cast<unsigned char>(w & 255));
            for (int c = 0; c < 4; ++c) {
                for (int x = 0; x < w; ++x) plane[static_cast<size_t>(x)] = rgbe[static_cast<size_t>(x) * 4 + c];
                RleComponent(plane.data(), w, line);
            }
        }
        if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) return false;
    }
    return true;
}

}  // namespace

bool SaveHdrImage(const HdrImage& img, const PathString& path, HdrSaveFormat format, std::string& error) {
    if (img.empty()) { error = "There is no image to save."; return false; }
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) { error = "The file could not be created. Check that the folder exists and isn't read-only."; return false; }

    bool ok = false;
    try {
        if (format == HdrSaveFormat::Radiance) {
            ok = SaveRadiance(img, f);
            if (!ok) error = "Writing the file failed (is the disk full?).";
        } else {
            ok = SaveExr(img, f, format == HdrSaveFormat::ExrHalf, error);
        }
    } catch (const std::bad_alloc&) {
        ok = false;
        error = "Not enough memory to save an image this large.";
    }
    const bool closed = std::fclose(f) == 0;
    if (ok && !closed) { ok = false; error = "Writing the file failed (is the disk full?)."; }
    if (!ok) RemoveFile(path);
    return ok;
}
