// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "exr_doc.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <unordered_set>

#include "parallel.h"
#include "tinyexr.h"   // implementation is compiled in image_io.cpp

namespace {

// Half-float -> float, all 65536 values precomputed once.
struct HalfTable {
    float v[65536];
    HalfTable() {
        for (uint32_t h = 0; h < 65536; ++h) {
            const uint32_t sign = (h & 0x8000u) << 16;
            uint32_t exp = (h >> 10) & 0x1f;
            uint32_t man = h & 0x3ffu;
            uint32_t bits;
            if (exp == 0) {
                if (man == 0) {
                    bits = sign;                                   // zero
                } else {                                           // subnormal
                    exp = 1;
                    while (!(man & 0x400u)) { man <<= 1; --exp; }
                    man &= 0x3ffu;
                    bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
                }
            } else if (exp == 31) {
                bits = sign | 0x7f800000u | (man << 13);           // inf / NaN
            } else {
                bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
            }
            std::memcpy(&v[h], &bits, sizeof(float));
        }
    }
};
const HalfTable& Halves() { static HalfTable t; return t; }

inline float ReadSample(const unsigned char* base, int type, size_t i, const HalfTable& ht) {
    switch (type) {
        case TINYEXR_PIXELTYPE_HALF: return ht.v[reinterpret_cast<const uint16_t*>(base)[i]];
        case TINYEXR_PIXELTYPE_FLOAT: return reinterpret_cast<const float*>(base)[i];
        default: return static_cast<float>(reinterpret_cast<const uint32_t*>(base)[i]);
    }
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// tinyexr can't decode channels stored at reduced resolution (the
// luminance/chroma "YC" layout): it would give a black image, so say so instead.
bool HasSubsampledChannels(const EXRHeader& h) {
    for (int c = 0; c < h.num_channels; ++c)
        if (h.channels[c].x_sampling != 1 || h.channels[c].y_sampling != 1) return true;
    return false;
}

const char kSubsampledError[] =
    "This EXR stores colour as luminance + chroma at reduced resolution (YC), "
    "which isn't supported.";

std::string ExrErrorMessage(int ret, const std::string& detail, unsigned char flags) {
    if (flags & 0x08) return "This EXR contains deep data, which can't be shown as a normal image.";
    if (ret == TINYEXR_ERROR_UNSUPPORTED_FEATURE || detail.find("ompression") != std::string::npos)
        return "This EXR uses a feature that isn't supported (most likely DWAA/DWAB compression).";
    std::string e = "This EXR file could not be decoded";
    return e + (detail.empty() ? "." : ": " + detail);
}

// Reads the cryptomatte/<key>/name and /manifest attributes straight from the
// header bytes. (tinyexr keeps only its first 128 custom attributes, and
// renderers like Unreal write more than that.) Returns one list per part.
struct RawCrypto { std::string key, name, manifest; };

std::vector<std::vector<RawCrypto>> ScanCryptoAttributes(const std::vector<unsigned char>& d, bool multipart) {
    std::vector<std::vector<RawCrypto>> parts;
    size_t pos = 8;   // after magic number and version
    auto readStr = [&](std::string& out) {
        size_t start = pos;
        while (pos < d.size() && d[pos] != 0) ++pos;
        if (pos >= d.size()) return false;
        out.assign(reinterpret_cast<const char*>(&d[start]), pos - start);
        ++pos;
        return true;
    };
    for (;;) {
        if (pos >= d.size()) break;
        if (multipart && d[pos] == 0) break;          // empty header ends the list
        std::vector<RawCrypto> found;
        for (;;) {
            std::string name, type;
            if (!readStr(name)) return parts;
            if (name.empty()) break;                    // end of this header
            if (!readStr(type) || pos + 4 > d.size()) return parts;
            int32_t size;
            std::memcpy(&size, &d[pos], 4);
            pos += 4;
            if (size < 0 || pos + static_cast<size_t>(size) > d.size()) return parts;
            if (name.compare(0, 12, "cryptomatte/") == 0 && type == "string") {
                const size_t slash = name.find('/', 12);
                if (slash != std::string::npos) {
                    const std::string key = name.substr(12, slash - 12), field = name.substr(slash + 1);
                    auto it = std::find_if(found.begin(), found.end(), [&](const RawCrypto& c) { return c.key == key; });
                    if (it == found.end()) { found.push_back({key, "", ""}); it = found.end() - 1; }
                    std::string value(reinterpret_cast<const char*>(&d[pos]), static_cast<size_t>(size));
                    if (field == "name") it->name = value;
                    else if (field == "manifest") it->manifest = std::move(value);
                }
            }
            pos += static_cast<size_t>(size);
        }
        parts.push_back(std::move(found));
        if (!multipart) break;
    }
    return parts;
}

// Manifest: a JSON object of "name": "hex id" pairs.
std::unordered_map<uint32_t, std::string> ParseManifest(const std::string& j) {
    std::unordered_map<uint32_t, std::string> out;
    size_t i = 0;
    auto skipWs = [&]() { while (i < j.size() && std::isspace(static_cast<unsigned char>(j[i]))) ++i; };
    auto readString = [&](std::string& s) {
        skipWs();
        if (i >= j.size() || j[i] != '"') return false;
        ++i;
        s.clear();
        while (i < j.size() && j[i] != '"') {
            if (j[i] == '\\' && i + 1 < j.size()) {
                char e = j[++i];
                if (e == 'n') s += '\n';
                else if (e == 't') s += '\t';
                else if (e == 'u' && i + 4 < j.size()) {
                    unsigned cp = static_cast<unsigned>(std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) s += static_cast<char>(cp);
                    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                } else s += e;
                ++i;
            } else {
                s += j[i++];
            }
        }
        if (i >= j.size()) return false;
        ++i;   // closing quote
        return true;
    };
    skipWs();
    if (i >= j.size() || j[i] != '{') return out;
    ++i;
    for (;;) {
        std::string name, hex;
        if (!readString(name)) break;
        skipWs();
        if (i >= j.size() || j[i] != ':') break;
        ++i;
        if (!readString(hex)) break;
        char* end = nullptr;
        const unsigned long id = std::strtoul(hex.c_str(), &end, 16);
        if (end && *end == '\0' && !hex.empty()) out[static_cast<uint32_t>(id)] = name;
        skipWs();
        if (i < j.size() && j[i] == ',') { ++i; continue; }
        break;
    }
    return out;
}

inline uint32_t FloatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

bool Has(const std::string& upperName, const char* word) { return upperName.find(word) != std::string::npos; }

// Decides from the layer's name and channels whether it holds data (depth,
// normals, vectors) rather than colour.
LayerView ClassifyView(const ExrLayer& L) {
    const std::string n = Upper(L.name);
    std::string last = n;   // name after the last '/' (multi-part "part/pass")
    if (size_t slash = n.find_last_of('/'); slash != std::string::npos) last = n.substr(slash + 1);
    const int comps = static_cast<int>(L.channels.size());
    if (Has(last, "DEPTH") || Has(last, "DISTANCE") || last == "Z" || last == "ZDEPTH" || L.channels == "Z")
        return LayerView::Depth;
    if (Has(last, "NORMAL") || last == "N" || last == "NN") return LayerView::Normal;
    if (Has(last, "MOTION") || Has(last, "VELOCITY") || Has(last, "VECTOR") || Has(last, "FLOW") ||
        Has(last, "FORWARD") || Has(last, "BACKWARD") || last == "P" || Has(last, "POSITION") || L.channels == "XYZ")
        return LayerView::Vector;
    // Two-channel layers that aren't colour (e.g. UV, motion) are vectors too.
    if (comps == 2 && L.channels != "RA" && L.channels != "GA") return LayerView::Vector;
    return LayerView::Color;
}

// Value below which `fraction` of the finite samples lie (approximate:
// uses up to ~1M evenly spaced samples).
float Percentile(const float* v, size_t n, size_t stride, int offset, float fraction, bool absolute = false) {
    std::vector<float> s;
    const size_t step = std::max<size_t>(1, n / 1000000);
    s.reserve(n / step + 1);
    for (size_t i = 0; i < n; i += step) {
        float x = v[i * stride + offset];
        if (!std::isfinite(x)) continue;
        s.push_back(absolute ? std::fabs(x) : x);
    }
    if (s.empty()) return 0.0f;
    const size_t k = std::min(s.size() - 1, static_cast<size_t>(fraction * (s.size() - 1)));
    std::nth_element(s.begin(), s.begin() + static_cast<long>(k), s.end());
    return s[k];
}

// Depth: near = white, far = black. "Background" pixels are left out of the
// range and shown black (far): renderers mark empty areas either with 0 or
// with a huge/infinite distance, so a large share of pixels at exactly 0 or at
// the very largest value counts as background.
void DepthView(float* px, size_t n, bool invert) {
    float maxv = -1e30f;
    size_t zeros = 0, positive = 0;
    for (size_t i = 0; i < n; ++i) {
        const float x = px[i * 3];
        if (!std::isfinite(x)) continue;
        if (x > maxv) maxv = x;
        if (x == 0.0f) ++zeros; else if (x > 0.0f) ++positive;
    }
    size_t atMax = 0;
    for (size_t i = 0; i < n; ++i) if (!std::isfinite(px[i * 3]) || (maxv > 0 && px[i * 3] >= maxv * 0.999f)) ++atMax;
    const bool farBackground = atMax > n / 200 && maxv > 0;
    const bool zeroBackground = zeros > n / 200 && positive > 0;
    auto isBackground = [&](float x) {
        return !std::isfinite(x) || (farBackground && x >= maxv * 0.999f) || (zeroBackground && x == 0.0f);
    };

    std::vector<float> s;
    const size_t step = std::max<size_t>(1, n / 1000000);
    for (size_t i = 0; i < n; i += step) if (!isBackground(px[i * 3])) s.push_back(px[i * 3]);
    float nearV = 0, farV = 1;
    if (!s.empty()) {
        std::sort(s.begin(), s.end());
        nearV = s[static_cast<size_t>(0.001 * (s.size() - 1))];
        farV = s[static_cast<size_t>(0.999 * (s.size() - 1))];
    }
    if (farV <= nearV) farV = nearV + 1e-6f;
    const float inv = 1.0f / (farV - nearV);
    ParallelRows(static_cast<int>(n / 1024 + 1), [&](int b0, int b1) {
        for (size_t i = static_cast<size_t>(b0) * 1024, e = std::min(n, static_cast<size_t>(b1) * 1024); i < e; ++i) {
            const float x = px[i * 3];
            const float t = isBackground(x) ? 1.0f : std::clamp((x - nearV) * inv, 0.0f, 1.0f);
            const float g = invert ? t : 1.0f - t;
            px[i * 3] = px[i * 3 + 1] = px[i * 3 + 2] = g;
        }
    }, 1024);
}

// Normals: stored as -1..+1 they're shown as n * 0.5 + 0.5 (the usual
// normal-map colours); if they're already stored 0..1 they're shown as they are.
void NormalView(float* px, size_t n) {
    const float lo = std::min({Percentile(px, n, 3, 0, 0.01f), Percentile(px, n, 3, 1, 0.01f), Percentile(px, n, 3, 2, 0.01f)});
    const bool signedNormals = lo < -0.02f;
    ParallelRows(static_cast<int>(n / 1024 + 1), [&](int b0, int b1) {
        for (size_t i = static_cast<size_t>(b0) * 1024 * 3, e = std::min(n * 3, static_cast<size_t>(b1) * 1024 * 3); i < e; ++i) {
            float x = std::isfinite(px[i]) ? px[i] : 0.0f;
            if (signedNormals) x = x * 0.5f + 0.5f;
            px[i] = std::clamp(x, 0.0f, 1.0f);
        }
    }, 1024);
}

// Vectors / positions: zero is mid-grey; scaled so 99% of values fit.
void VectorView(float* px, size_t n, bool hasBlue) {
    float scale = std::max({Percentile(px, n, 3, 0, 0.99f, true), Percentile(px, n, 3, 1, 0.99f, true),
                            hasBlue ? Percentile(px, n, 3, 2, 0.99f, true) : 0.0f});
    if (scale < 1e-12f) scale = 1.0f;
    const float k = 0.5f / scale;
    ParallelRows(static_cast<int>(n / 1024 + 1), [&](int b0, int b1) {
        for (size_t i = static_cast<size_t>(b0) * 1024, e = std::min(n, static_cast<size_t>(b1) * 1024); i < e; ++i) {
            for (int c = 0; c < 3; ++c) {
                float x = px[i * 3 + c];
                if (c == 2 && !hasBlue) { px[i * 3 + c] = 0.0f; continue; }
                x = std::isfinite(x) ? x * k + 0.5f : 0.5f;
                px[i * 3 + c] = std::clamp(x, 0.0f, 1.0f);
            }
        }
    }, 1024);
}

// A distinct, fairly bright display colour for an object ID.
void IdColor(uint32_t id, float rgb[3]) {
    uint32_t h = id;                       // scramble the bits (MurmurHash3 finaliser)
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    const float hue = (h & 0xffffu) / 65536.0f * 6.0f;
    const float sat = 0.45f + ((h >> 16) & 0xffu) / 255.0f * 0.45f;
    const float val = 0.60f + ((h >> 24) & 0xffu) / 255.0f * 0.40f;
    const int sector = static_cast<int>(hue);
    const float f = hue - sector, p = val * (1 - sat), q = val * (1 - sat * f), t = val * (1 - sat * (1 - f));
    float r, g, b;
    switch (sector % 6) {
        case 0: r = val; g = t; b = p; break;
        case 1: r = q; g = val; b = p; break;
        case 2: r = p; g = val; b = t; break;
        case 3: r = p; g = q; b = val; break;
        case 4: r = t; g = p; b = val; break;
        default: r = val; g = p; b = q; break;
    }
    rgb[0] = r; rgb[1] = g; rgb[2] = b;
}

}  // namespace

struct ExrDocument::Part {
    EXRHeader* header = nullptr;
    EXRImage image;
    std::string name;
    std::vector<int> tileIndex;   // level-0 tile number for each tile position (tiled parts)
    int tilesX = 0;
    Part() { InitEXRImage(&image); }
    ~Part() {
        FreeEXRImage(&image);
        if (header) { FreeEXRHeader(header); std::free(header); }
    }
};

ExrDocument::~ExrDocument() = default;

std::shared_ptr<ExrDocument> ExrDocument::Load(const std::vector<unsigned char>& data, std::string& error) {
    std::shared_ptr<ExrDocument> doc(new ExrDocument());
    const unsigned char flags = data.size() > 5 ? data[5] : 0;

    EXRVersion version;
    if (ParseEXRVersionFromMemory(&version, data.data(), data.size()) != TINYEXR_SUCCESS) {
        error = "This EXR file has a damaged header.";
        return nullptr;
    }
    if (version.non_image && !version.multipart) {
        error = ExrErrorMessage(0, "", 0x08);
        return nullptr;
    }

    const char* err = nullptr;
    auto takeErr = [&]() { std::string s = err ? err : ""; if (err) FreeEXRErrorMessage(err); err = nullptr; return s; };

    if (!version.multipart) {
        auto part = std::make_unique<Part>();
        part->header = static_cast<EXRHeader*>(std::malloc(sizeof(EXRHeader)));
        if (!part->header) throw std::bad_alloc();
        InitEXRHeader(part->header);
        int ret = ParseEXRHeaderFromMemory(part->header, &version, data.data(), data.size(), &err);
        if (ret != TINYEXR_SUCCESS) { error = ExrErrorMessage(ret, takeErr(), flags); return nullptr; }
        if (HasSubsampledChannels(*part->header)) { error = kSubsampledError; return nullptr; }
        // requested_pixel_types start out equal to the stored types: half stays half.
        ret = LoadEXRImageFromMemory(&part->image, part->header, data.data(), data.size(), &err);
        if (ret != TINYEXR_SUCCESS) { error = ExrErrorMessage(ret, takeErr(), flags); return nullptr; }
        part->name = part->header->name;
        doc->parts_.push_back(std::move(part));
    } else {
        EXRHeader** headers = nullptr;
        int count = 0;
        int ret = ParseEXRMultipartHeaderFromMemory(&headers, &count, &version, data.data(), data.size(), &err);
        if (ret != TINYEXR_SUCCESS) { error = ExrErrorMessage(ret, takeErr(), flags); return nullptr; }
        bool deep = false;
        for (int i = 0; i < count; ++i) {
            auto part = std::make_unique<Part>();
            part->header = headers[i];        // the part now owns it
            part->name = headers[i]->name;
            if (headers[i]->non_image) deep = true;
            doc->parts_.push_back(std::move(part));
        }
        std::free(headers);
        if (deep) { error = ExrErrorMessage(0, "", 0x08); return nullptr; }
        for (const auto& part : doc->parts_)
            if (HasSubsampledChannels(*part->header)) { error = kSubsampledError; return nullptr; }

        std::vector<EXRImage> images(static_cast<size_t>(count));
        std::vector<const EXRHeader*> hp(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) { InitEXRImage(&images[i]); hp[i] = doc->parts_[i]->header; }
        ret = LoadEXRMultipartImageFromMemory(images.data(), hp.data(), static_cast<unsigned>(count),
                                              data.data(), data.size(), &err);
        if (ret != TINYEXR_SUCCESS) {
            for (auto& im : images) FreeEXRImage(&im);
            error = ExrErrorMessage(ret, takeErr(), flags);
            return nullptr;
        }
        for (int i = 0; i < count; ++i) doc->parts_[i]->image = images[i];   // ownership moves to the part
    }

    for (auto& part : doc->parts_) {
        const EXRHeader& h = *part->header;
        if (!h.tiled || h.tile_size_x <= 0 || h.tile_size_y <= 0) continue;
        part->tilesX = (part->image.width + h.tile_size_x - 1) / h.tile_size_x;
        const int tilesY = (part->image.height + h.tile_size_y - 1) / h.tile_size_y;
        part->tileIndex.assign(static_cast<size_t>(part->tilesX) * tilesY, -1);
        for (int t = 0; t < part->image.num_tiles; ++t) {
            const EXRTile& tile = part->image.tiles[t];
            if (tile.level_x == 0 && tile.level_y == 0 && tile.offset_x < part->tilesX && tile.offset_y < tilesY)
                part->tileIndex[static_cast<size_t>(tile.offset_y) * part->tilesX + tile.offset_x] = t;
        }
    }

    std::vector<std::vector<CryptoMeta>> meta;
    for (auto& partList : ScanCryptoAttributes(data, version.multipart != 0)) {
        meta.emplace_back();
        for (auto& c : partList) meta.back().push_back({c.name, std::move(c.manifest)});
    }
    doc->FindLayers(meta);
    if (doc->layers_.empty()) {
        error = "This EXR has no image channels that can be shown.";
        return nullptr;
    }
    return doc;
}

std::string ExrDocument::DisplayName(const ExrLayer& l) {
    return l.name.empty() ? std::string("Main image") : l.name;
}

void ExrDocument::FindLayers(const std::vector<std::vector<CryptoMeta>>& meta) {
    layers_.clear();
    cryptos_.clear();
    const bool multi = parts_.size() > 1;
    for (int p = 0; p < static_cast<int>(parts_.size()); ++p) {
        const Part& part = *parts_[static_cast<size_t>(p)];
        const EXRHeader& h = *part.header;

        // Group channels by the text before the last '.' ("diffuse.R" -> "diffuse").
        struct Group { std::string prefix; std::vector<std::pair<std::string, int>> comps; };
        std::vector<Group> groups;
        for (int c = 0; c < h.num_channels; ++c) {
            const std::string full = h.channels[c].name;
            const size_t dot = full.find_last_of('.');
            const std::string prefix = dot == std::string::npos ? "" : full.substr(0, dot);
            const std::string comp = dot == std::string::npos ? full : full.substr(dot + 1);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& gr) { return gr.prefix == prefix; });
            if (it == groups.end()) { groups.push_back({prefix, {}}); it = groups.end() - 1; }
            it->comps.push_back({Upper(comp), c});
        }

        // Cryptomatte: rank groups "<name>00", "<name>01", ... with R,G,B,A =
        // (id, coverage, id, coverage). Names come from the header metadata;
        // groups called Crypto...NN are recognised even without it.
        auto channelOf = [&](const Group& gr, const char* comp) {
            for (const auto& cc : gr.comps) if (cc.first == comp) return cc.second;
            return -1;
        };
        auto isRank = [&](const Group& gr, std::string& base, int& rank) {
            const std::string& pf = gr.prefix;
            if (pf.size() < 3 || !std::isdigit(static_cast<unsigned char>(pf[pf.size() - 1])) ||
                !std::isdigit(static_cast<unsigned char>(pf[pf.size() - 2])))
                return false;
            if (channelOf(gr, "R") < 0 || channelOf(gr, "G") < 0) return false;
            base = pf.substr(0, pf.size() - 2);
            rank = std::atoi(pf.c_str() + pf.size() - 2);
            return true;
        };
        std::vector<std::string> cryptoNames;
        if (p < static_cast<int>(meta.size()))
            for (const auto& m : meta[static_cast<size_t>(p)]) if (!m.name.empty()) cryptoNames.push_back(m.name);
        for (const Group& gr : groups) {
            std::string base; int rank;
            if (isRank(gr, base, rank) && rank == 0 && Upper(base).find("CRYPTO") != std::string::npos &&
                std::find(cryptoNames.begin(), cryptoNames.end(), base) == cryptoNames.end())
                cryptoNames.push_back(base);
        }
        std::unordered_map<std::string, int> cryptoOfBase;
        for (const std::string& cname : cryptoNames) {
            std::vector<std::pair<int, const Group*>> ranks;
            for (const Group& gr : groups) {
                std::string base; int rank;
                if (isRank(gr, base, rank) && base == cname) ranks.push_back({rank, &gr});
            }
            if (ranks.empty()) continue;
            std::sort(ranks.begin(), ranks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            CryptoSet cs;
            cs.name = cname;
            cs.part = p;
            for (const auto& rk : ranks) {
                cs.ranks.push_back({channelOf(*rk.second, "R"), channelOf(*rk.second, "G")});
                const int b = channelOf(*rk.second, "B"), a = channelOf(*rk.second, "A");
                if (b >= 0 && a >= 0) cs.ranks.push_back({b, a});
            }
            if (p < static_cast<int>(meta.size()))
                for (const auto& m : meta[static_cast<size_t>(p)])
                    if (m.name == cname && !m.manifest.empty()) cs.names = ParseManifest(m.manifest);
            cryptoOfBase[cname] = static_cast<int>(cryptos_.size());
            cryptos_.push_back(std::move(cs));
        }

        for (const Group& gr : groups) {
            if (gr.comps.empty()) continue;
            {
                std::string base; int rank;
                if (isRank(gr, base, rank) && cryptoOfBase.count(base)) {
                    // Show one layer for the whole Cryptomatte, at its first rank's place.
                    const int ci = cryptoOfBase[base];
                    const bool already = std::any_of(layers_.begin(), layers_.end(), [&](const ExrLayer& l) { return l.crypto == ci; });
                    if (!already) {
                        ExrLayer L;
                        L.part = p;
                        L.width = part.image.width;
                        L.height = part.image.height;
                        L.crypto = ci;
                        L.name = multi && !part.name.empty() && part.name.rfind(base, 0) != 0 ? part.name + "/" + base : base;
                        L.channels = "Cryptomatte";
                        layers_.push_back(L);
                    }
                    continue;
                }
            }
            auto find = [&](std::initializer_list<const char*> names) {
                for (const auto& cc : gr.comps)
                    for (const char* n : names)
                        if (cc.first == n) return cc.second;
                return -1;
            };
            ExrLayer L;
            L.part = p;
            L.width = part.image.width;
            L.height = part.image.height;
            std::string name = multi ? part.name : std::string();
            // "forward_left" + "forward" reads better as just "forward_left".
            if (!gr.prefix.empty() && name.rfind(gr.prefix, 0) != 0)
                name = name.empty() ? gr.prefix : name + "/" + gr.prefix;
            if (multi && name.empty()) name = "part " + std::to_string(p + 1);
            L.name = name;

            const int r = find({"R", "RED"}), gch = find({"G", "GREEN"}), b = find({"B", "BLUE"});
            const int a = find({"A", "ALPHA"});
            const int x = find({"X"}), y = find({"Y"}), z = find({"Z"});
            if (r >= 0 && gch >= 0 && b >= 0) {
                L.src[0] = r; L.src[1] = gch; L.src[2] = b;
                L.channels = a >= 0 ? "RGBA" : "RGB";
            } else if (x >= 0 && y >= 0 && z >= 0) {
                L.src[0] = x; L.src[1] = y; L.src[2] = z;
                L.channels = "XYZ";
            } else if (y >= 0) {
                L.src[0] = L.src[1] = L.src[2] = y;
                L.channels = "Y";
            } else {
                // Anything else: the first up to three channels (alpha last resort).
                std::vector<std::pair<std::string, int>> use;
                for (const auto& cc : gr.comps) if (cc.second != a) use.push_back(cc);
                if (use.empty()) use = gr.comps;
                if (use.size() == 1) {
                    L.src[0] = L.src[1] = L.src[2] = use[0].second;
                } else {
                    for (size_t k = 0; k < std::min<size_t>(3, use.size()); ++k) L.src[k] = use[k].second;
                }
                for (size_t k = 0; k < std::min<size_t>(3, use.size()); ++k) L.channels += use[k].first;
            }
            L.view = ClassifyView(L);
            layers_.push_back(L);
        }
    }

    // Prefer the unnamed RGB image of the first part, then any RGB layer.
    defaultLayer_ = 0;
    int anyRgb = -1;
    for (int i = 0; i < static_cast<int>(layers_.size()); ++i) {
        const ExrLayer& l = layers_[static_cast<size_t>(i)];
        const bool rgb = l.channels.rfind("RGB", 0) == 0;
        if (rgb && anyRgb < 0) anyRgb = i;
        if (rgb && l.part == 0 && (l.name.empty() || l.name == parts_[0]->name)) { defaultLayer_ = i; return; }
    }
    if (anyRgb >= 0) defaultLayer_ = anyRgb;
}

void ExrDocument::ConvertChannel(const Part& p, int ch, float* out, int stride, int offset) const {
    const HalfTable& ht = Halves();
    const EXRHeader& h = *p.header;
    const int type = h.pixel_types[ch];
    const size_t w = static_cast<size_t>(p.image.width), H = static_cast<size_t>(p.image.height);

    if (!h.tiled) {
        const unsigned char* src = p.image.images[ch];
        ParallelRows(static_cast<int>(H), [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w; i < e; ++i)
                out[i * stride + offset] = ReadSample(src, type, i, ht);
        }, static_cast<long long>(w));
        return;
    }

    // Tiled: copy each level-0 tile into place. Tile buffers are tile_size_x wide.
    const size_t tsx = static_cast<size_t>(h.tile_size_x), tsy = static_cast<size_t>(h.tile_size_y);
    ParallelRows(p.image.num_tiles, [&](int t0, int t1) {
        for (int t = t0; t < t1; ++t) {
            const EXRTile& tile = p.image.tiles[t];
            if (tile.level_x != 0 || tile.level_y != 0) continue;
            const unsigned char* src = tile.images[ch];
            const size_t ox = static_cast<size_t>(tile.offset_x) * tsx, oy = static_cast<size_t>(tile.offset_y) * tsy;
            for (size_t j = 0; j < static_cast<size_t>(tile.height); ++j) {
                const size_t y = oy + j;
                if (y >= H) break;
                for (size_t i = 0; i < static_cast<size_t>(tile.width); ++i) {
                    const size_t x = ox + i;
                    if (x >= w) break;
                    out[(y * w + x) * stride + offset] = ReadSample(src, type, i + j * tsx, ht);
                }
            }
        }
    }, static_cast<long long>(tsx * tsy));
}

const char* ExrDocument::ViewName(LayerView v) {
    switch (v) {
        case LayerView::Depth: return "depth view";
        case LayerView::Normal: return "normal view";
        case LayerView::Vector: return "vector view";
        default: return "";
    }
}

void ExrDocument::RawAt(int index, int x, int y, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (index < 0 || index >= static_cast<int>(layers_.size())) return;
    const ExrLayer& L = layers_[static_cast<size_t>(index)];
    const Part& p = *parts_[static_cast<size_t>(L.part)];
    if (x < 0 || y < 0 || x >= p.image.width || y >= p.image.height) return;
    for (int c = 0; c < 3; ++c)
        if (L.src[c] >= 0) out[c] = SampleRaw(p, L.src[c], x, y);
}

bool ExrDocument::BuildLayer(int index, HdrImage& out, std::string& error, const ViewOptions& opts) const {
    out.clear();
    if (index < 0 || index >= static_cast<int>(layers_.size())) { error = "No such layer."; return false; }
    const ExrLayer& L = layers_[static_cast<size_t>(index)];
    if (L.crypto >= 0) return BuildCryptoView(L.crypto, {}, out, error);
    const Part& p = *parts_[static_cast<size_t>(L.part)];
    const size_t n = static_cast<size_t>(p.image.width) * static_cast<size_t>(p.image.height);
    if (n == 0) { error = "This layer is empty."; return false; }

    float* px = static_cast<float*>(std::calloc(n * 3, sizeof(float)));
    if (!px) { error = "Not enough memory for an image this large."; return false; }

    const bool gray = L.src[0] >= 0 && L.src[0] == L.src[1] && L.src[1] == L.src[2];
    if (gray) {
        ConvertChannel(p, L.src[0], px, 3, 0);
        ParallelRows(p.image.height, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * p.image.width, e = static_cast<size_t>(y1) * p.image.width; i < e; ++i)
                px[i * 3 + 1] = px[i * 3 + 2] = px[i * 3];
        }, p.image.width);
    } else {
        for (int c = 0; c < 3; ++c)
            if (L.src[c] >= 0) ConvertChannel(p, L.src[c], px, 3, c);
    }
    switch (L.view) {
        case LayerView::Depth: DepthView(px, n, opts.invertDepth); out.displayReferred = true; break;
        case LayerView::Normal: NormalView(px, n); out.displayReferred = true; break;
        case LayerView::Vector: VectorView(px, n, L.src[2] >= 0); out.displayReferred = true; break;
        case LayerView::Color: break;
    }
    out.pixels.reset(px);
    out.width = p.image.width;
    out.height = p.image.height;
    out.format = "EXR";
    return true;
}

float ExrDocument::SampleRaw(const Part& p, int ch, int x, int y) const {
    const HalfTable& ht = Halves();
    const EXRHeader& h = *p.header;
    const int type = h.pixel_types[ch];
    if (!h.tiled) return ReadSample(p.image.images[ch], type, static_cast<size_t>(y) * p.image.width + x, ht);
    const int tx = x / h.tile_size_x, ty = y / h.tile_size_y;
    const size_t slot = static_cast<size_t>(ty) * p.tilesX + tx;
    if (slot >= p.tileIndex.size() || p.tileIndex[slot] < 0) return 0.0f;
    const EXRTile& tile = p.image.tiles[p.tileIndex[slot]];
    const size_t i = static_cast<size_t>(x - tx * h.tile_size_x) + static_cast<size_t>(y - ty * h.tile_size_y) * h.tile_size_x;
    return ReadSample(tile.images[ch], type, i, ht);
}

std::vector<CryptoHit> ExrDocument::CryptoAt(int set, int x, int y) const {
    std::vector<CryptoHit> hits;
    if (set < 0 || set >= CryptoCount()) return hits;
    const CryptoSet& cs = cryptos_[static_cast<size_t>(set)];
    const Part& p = *parts_[static_cast<size_t>(cs.part)];
    if (x < 0 || y < 0 || x >= p.image.width || y >= p.image.height) return hits;
    for (const auto& rk : cs.ranks) {
        const float cov = SampleRaw(p, rk.second, x, y);
        if (!(cov > 0.0f)) continue;
        const uint32_t id = FloatBits(SampleRaw(p, rk.first, x, y));
        auto it = std::find_if(hits.begin(), hits.end(), [&](const CryptoHit& h) { return h.id == id; });
        if (it != hits.end()) it->coverage += cov;
        else hits.push_back({id, cov});
    }
    std::sort(hits.begin(), hits.end(), [](const CryptoHit& a, const CryptoHit& b) { return a.coverage > b.coverage; });
    return hits;
}

std::string ExrDocument::CryptoName(int set, uint32_t id) const {
    if (set >= 0 && set < CryptoCount()) {
        const auto& names = cryptos_[static_cast<size_t>(set)].names;
        auto it = names.find(id);
        if (it != names.end()) return it->second;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "ID %08x", id);
    return buf;
}

bool ExrDocument::BuildCryptoView(int set, const std::vector<uint32_t>& isolate, HdrImage& out, std::string& error) const {
    out.clear();
    if (set < 0 || set >= CryptoCount()) { error = "No such Cryptomatte."; return false; }
    const CryptoSet& cs = cryptos_[static_cast<size_t>(set)];
    const Part& p = *parts_[static_cast<size_t>(cs.part)];
    const int w = p.image.width, h = p.image.height;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (n == 0) { error = "This layer is empty."; return false; }

    float* px = static_cast<float*>(std::calloc(n * 3, sizeof(float)));
    if (!px) { error = "Not enough memory for an image this large."; return false; }
    std::vector<float> ids, covs;
    try { ids.resize(n); covs.resize(n); } catch (...) {
        std::free(px);
        error = "Not enough memory for an image this large.";
        return false;
    }
    const std::unordered_set<uint32_t> wanted(isolate.begin(), isolate.end());

    for (const auto& rk : cs.ranks) {
        ConvertChannel(p, rk.first, ids.data(), 1, 0);
        ConvertChannel(p, rk.second, covs.data(), 1, 0);
        ParallelRows(h, [&](int y0, int y1) {
            for (size_t i = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w; i < e; ++i) {
                const float cov = covs[i];
                if (!(cov > 0.0f)) continue;
                const uint32_t id = FloatBits(ids[i]);
                if (wanted.empty()) {
                    float c[3];
                    IdColor(id, c);
                    px[i * 3 + 0] += cov * c[0];
                    px[i * 3 + 1] += cov * c[1];
                    px[i * 3 + 2] += cov * c[2];
                } else if (wanted.count(id)) {
                    px[i * 3 + 0] += cov;
                    px[i * 3 + 1] += cov;
                    px[i * 3 + 2] += cov;
                }
            }
        }, w);
    }
    out.pixels.reset(px);
    out.width = w;
    out.height = h;
    out.format = "EXR";
    out.displayReferred = true;
    return true;
}
