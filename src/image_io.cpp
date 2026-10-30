// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image_io.h"
#include "exr_doc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

// --- third-party decoders (compiled into this file) -------------------------

#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 1     // decode EXR blocks on several cores
#define TINYEXR_USE_OPENMP 0     // no OpenMP runtime DLL needed
#include "miniz.h"
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define STBI_ONLY_HDR
#define STBI_NO_STDIO            // we read files ourselves (Unicode paths)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// -----------------------------------------------------------------------------

namespace {

bool ReadWholeFile(const PathString& path, std::vector<unsigned char>& data, std::string& error) {
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) { error = "The file could not be opened."; return false; }

#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    std::fseek(f, 0, SEEK_END);
    long long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
#endif
    if (size <= 0) { std::fclose(f); error = "The file is empty."; return false; }

    try {
        data.resize(static_cast<size_t>(size));
    } catch (...) {
        std::fclose(f);
        error = "Not enough memory to read the file.";
        return false;
    }
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) { error = "The file could not be read completely."; return false; }
    return true;
}

bool LooksLikeExr(const std::vector<unsigned char>& d) {
    return d.size() >= 4 && d[0] == 0x76 && d[1] == 0x2f && d[2] == 0x31 && d[3] == 0x01;
}

bool LooksLikeRadiance(const std::vector<unsigned char>& d) {
    return d.size() >= 6 && d[0] == '#' && d[1] == '?';   // "#?RADIANCE" or "#?RGBE"
}

bool LoadExr(const std::vector<unsigned char>& data, HdrImage& out, std::string& error,
             std::shared_ptr<ExrDocument>* keepDoc) {
    std::shared_ptr<ExrDocument> doc = ExrDocument::Load(data, error);
    if (!doc) return false;
    if (!doc->BuildLayer(doc->DefaultLayer(), out, error)) return false;
    // Keep the decoded channels only when there is something else to switch to.
    if (keepDoc && doc->Layers().size() > 1) *keepDoc = std::move(doc);
    return true;
}

// stb_image's reader: kept as a fallback for unusual Radiance files (other
// orientations); it can't go past 2 GB of decoded pixels.
bool LoadRadianceStb(const std::vector<unsigned char>& data, HdrImage& out, std::string& error) {
    if (data.size() > 0x7fffffffu) { error = "This HDR file is too large."; return false; }
    int w = 0, h = 0, comp = 0;
    float* px = stbi_loadf_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp, 3);
    if (!px) {
        error = "This HDR file could not be decoded";
        const char* reason = stbi_failure_reason();
        if (reason) { error += ": "; error += reason; }
        else error += ".";
        return false;
    }
    out.pixels.reset(px);   // stb_image allocates with malloc, so we can adopt it directly
    out.width = w;
    out.height = h;
    out.format = "HDR";
    return true;
}

inline void RgbeToFloat(const unsigned char* p, float* out) {
    if (p[3] == 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    const float f = std::ldexp(1.0f, static_cast<int>(p[3]) - (128 + 8));
    out[0] = p[0] * f;
    out[1] = p[1] * f;
    out[2] = p[2] * f;
}

// One scanline of RGBE pixels (flat, old-style RLE or new-style RLE) into `row`
// (w*4 bytes). Advances `pos`. False if the data is cut short or corrupt.
bool ReadRgbeScanline(const unsigned char* d, size_t size, size_t& pos, int w, unsigned char* row) {
    if (pos + 4 <= size && w >= 8 && w < 32768 && d[pos] == 2 && d[pos + 1] == 2 && !(d[pos + 2] & 0x80) &&
        ((d[pos + 2] << 8) | d[pos + 3]) == w) {
        pos += 4;   // new-style RLE: each of R, G, B, E run-length encoded separately
        for (int c = 0; c < 4; ++c) {
            int x = 0;
            while (x < w) {
                if (pos >= size) return false;
                int n = d[pos++];
                if (n > 128) {                       // a run
                    n -= 128;
                    if (pos >= size || x + n > w) return false;
                    const unsigned char v = d[pos++];
                    for (int k = 0; k < n; ++k) row[(x + k) * 4 + c] = v;
                } else {                             // literal bytes
                    if (n == 0 || pos + n > size || x + n > w) return false;
                    for (int k = 0; k < n; ++k) row[(x + k) * 4 + c] = d[pos + k];
                    pos += static_cast<size_t>(n);
                }
                x += n;
            }
        }
        return true;
    }
    // Flat pixels, possibly with old-style runs (1,1,1,count repeats the previous pixel).
    int x = 0, shift = 0;
    while (x < w) {
        if (pos + 4 > size) return false;
        const unsigned char* q = d + pos;
        pos += 4;
        if (q[0] == 1 && q[1] == 1 && q[2] == 1) {
            if (x == 0) return false;
            const int n = static_cast<int>(q[3]) << shift;
            if (x + n > w) return false;
            for (int k = 0; k < n; ++k, ++x) std::memcpy(row + x * 4, row + (x - 1) * 4, 4);
            shift += 8;
        } else {
            std::memcpy(row + x * 4, q, 4);
            ++x;
            shift = 0;
        }
    }
    return true;
}

// Our own Radiance reader: no 2 GB limit (32K HDRIs and bigger), and with
// `reduce` > 1 it box-averages reduce x reduce blocks while decoding, so a
// thumbnail of a huge file never needs the full-size image in memory.
bool LoadRadiance(const std::vector<unsigned char>& data, HdrImage& out, std::string& error, int reduce = 1) {
    const unsigned char* d = data.data();
    const size_t size = data.size();
    // Header lines up to the blank line.
    size_t pos = 0;
    bool blank = false, xyze = false;
    while (pos < size && !blank) {
        const size_t start = pos;
        while (pos < size && d[pos] != '\n') ++pos;
        std::string line(reinterpret_cast<const char*>(d + start), pos - start);
        if (line.rfind("FORMAT=", 0) == 0 && line.find("xyze") != std::string::npos) xyze = true;
        blank = (pos == start);
        ++pos;
    }
    if (!blank || pos >= size) { error = "This HDR file's header is damaged."; return false; }
    if (xyze) { error = "This HDR file stores XYZ colours (32-bit_rle_xyze), which isn't supported."; return false; }
    const size_t lineStart = pos;
    while (pos < size && d[pos] != '\n') ++pos;
    std::string res(reinterpret_cast<const char*>(d + lineStart), pos - lineStart);
    ++pos;
    char a1[3] = {}, a2[3] = {};
    long long n1 = 0, n2 = 0;
    if (std::sscanf(res.c_str(), "%2s %lld %2s %lld", a1, &n1, a2, &n2) != 4) {
        error = "This HDR file's size line is damaged.";
        return false;
    }
    const std::string o1 = a1, o2 = a2;
    bool flipY = false;
    if (o1 == "-Y" && o2 == "+X") flipY = false;
    else if (o1 == "+Y" && o2 == "+X") flipY = true;
    else return LoadRadianceStb(data, out, error);   // rare orientations
    if (n1 <= 0 || n2 <= 0 || n1 > (1 << 20) || n2 > (1 << 20)) { error = "This HDR file has an invalid size."; return false; }
    const int h = static_cast<int>(n1), w = static_cast<int>(n2);

    reduce = std::max(1, reduce);
    const int ow = (w + reduce - 1) / reduce, oh = (h + reduce - 1) / reduce;
    const size_t values = static_cast<size_t>(ow) * oh * 3;
    float* px = static_cast<float*>(std::calloc(values, sizeof(float)));
    if (!px) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "Not enough memory to open an image this large (%d \xc3\x97 %d needs about %.1f GB).",
                      w, h, static_cast<double>(values) * 4.0 / (1024.0 * 1024.0 * 1024.0));
        error = buf;
        return false;
    }
    out.pixels.reset(px);
    out.width = ow;
    out.height = oh;
    out.format = "HDR";

    std::vector<unsigned char> row(static_cast<size_t>(w) * 4);
    std::vector<float> rgb(static_cast<size_t>(w) * 3);
    const float inv = 1.0f / static_cast<float>(reduce * reduce);
    for (int y = 0; y < h; ++y) {
        if (!ReadRgbeScanline(d, size, pos, w, row.data())) {
            out.clear();
            error = "This HDR file is damaged or cut short (scanline " + std::to_string(y + 1) + " of " +
                    std::to_string(h) + ").";
            return false;
        }
        const int oy = (flipY ? h - 1 - y : y) / reduce;
        float* dst = px + static_cast<size_t>(oy) * ow * 3;
        if (reduce == 1) {
            for (int x = 0; x < w; ++x) RgbeToFloat(&row[static_cast<size_t>(x) * 4], dst + static_cast<size_t>(x) * 3);
        } else {
            for (int x = 0; x < w; ++x) {
                float v[3];
                RgbeToFloat(&row[static_cast<size_t>(x) * 4], v);
                float* o = dst + static_cast<size_t>(x / reduce) * 3;
                o[0] += v[0] * inv; o[1] += v[1] * inv; o[2] += v[2] * inv;
            }
        }
    }
    if (reduce > 1 && (w % reduce || h % reduce)) {
        // Edge blocks are partial: scale them up to a true average.
        for (int oy = 0; oy < oh; ++oy) {
            const int cy = std::min(reduce, h - oy * reduce);
            for (int ox = 0; ox < ow; ++ox) {
                const int cx = std::min(reduce, w - ox * reduce);
                if (cx == reduce && cy == reduce) continue;
                const float k = static_cast<float>(reduce * reduce) / static_cast<float>(cx * cy);
                float* o = px + (static_cast<size_t>(oy) * ow + ox) * 3;
                o[0] *= k; o[1] *= k; o[2] *= k;
            }
        }
    }
    return true;
}

bool PeekExrSize(const std::vector<unsigned char>& data, int& w, int& h) {
    EXRVersion version;
    if (ParseEXRVersionFromMemory(&version, data.data(), data.size()) != TINYEXR_SUCCESS) return false;
    if (version.multipart || version.non_image) return false;
    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    int ret = ParseEXRHeaderFromMemory(&header, &version, data.data(), data.size(), &err);
    if (err) FreeEXRErrorMessage(err);
    if (ret != TINYEXR_SUCCESS) { FreeEXRHeader(&header); return false; }
    w = header.data_window.max_x - header.data_window.min_x + 1;
    h = header.data_window.max_y - header.data_window.min_y + 1;
    FreeEXRHeader(&header);
    return w > 0 && h > 0;
}

// Radiance header: text lines, a blank line, then e.g. "-Y 1024 +X 2048".
bool PeekRadianceSize(const std::vector<unsigned char>& data, int& w, int& h) {
    const size_t limit = std::min<size_t>(data.size(), 64 * 1024);
    size_t i = 0;
    bool blank = false;
    while (i < limit && !blank) {
        size_t start = i;
        while (i < limit && data[i] != '\n') ++i;
        blank = (i == start);
        ++i;   // skip '\n'
    }
    if (!blank || i >= limit) return false;
    std::string line;
    while (i < limit && data[i] != '\n') line += static_cast<char>(data[i++]);
    char a1[3] = {}, a2[3] = {};
    int n1 = 0, n2 = 0;
    if (std::sscanf(line.c_str(), "%2s %d %2s %d", a1, &n1, a2, &n2) != 4) return false;
    if (a1[1] == 'Y') { h = n1; w = n2; } else { w = n1; h = n2; }
    return w > 0 && h > 0;
}

}  // namespace

bool ReadFileBytes(const PathString& path, std::vector<unsigned char>& data, std::string& error) {
    return ReadWholeFile(path, data, error);
}

bool PeekImageSize(const std::vector<unsigned char>& data, int& width, int& height) {
    width = height = 0;
    if (LooksLikeExr(data)) return PeekExrSize(data, width, height);
    if (LooksLikeRadiance(data)) return PeekRadianceSize(data, width, height);
    return false;
}

bool DecodeHdrImage(const std::vector<unsigned char>& data, HdrImage& out, std::string& error,
                    std::shared_ptr<ExrDocument>* exrLayers) {
    out.clear();
    if (exrLayers) exrLayers->reset();
    if (LooksLikeExr(data)) return LoadExr(data, out, error, exrLayers);
    if (LooksLikeRadiance(data)) return LoadRadiance(data, out, error);
    error = "This doesn't look like an EXR or Radiance HDR file.";
    return false;
}

bool DecodeHdrImageReduced(const std::vector<unsigned char>& data, HdrImage& out, std::string& error, int maxWidth) {
    out.clear();
    if (LooksLikeRadiance(data)) {
        int w = 0, h = 0;
        const int reduce = (PeekRadianceSize(data, w, h) && maxWidth > 0) ? std::max(1, w / maxWidth) : 1;
        return LoadRadiance(data, out, error, reduce);
    }
    return DecodeHdrImage(data, out, error);
}

bool LoadHdrFile(const PathString& path, HdrImage& out, std::string& error) {
    out.clear();
    std::vector<unsigned char> data;
    if (!ReadWholeFile(path, data, error)) return false;
    return DecodeHdrImage(data, out, error);
}

bool IsSupportedExtension(const PathString& path) {
    size_t dot = path.find_last_of('.');
    if (dot == PathString::npos) return false;
    PathString ext = path.substr(dot + 1);
    for (auto& c : ext) {
#ifdef _WIN32
        c = static_cast<wchar_t>(std::towlower(c));
#else
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    }
#ifdef _WIN32
    return ext == L"exr" || ext == L"hdr";
#else
    return ext == "exr" || ext == "hdr";
#endif
}
