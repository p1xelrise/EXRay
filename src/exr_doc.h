// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// An opened EXR file with all its parts and channels, grouped into layers.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hdr_image.h"

// How a layer is shown. Colour passes go through exposure and tonemap like any
// image; data passes get a readable view instead (and the real values stay
// available for the pixel readout).
enum class LayerView {
    Color,    // beauty, diffuse, specular, AO, masks ...
    Depth,    // distance: near = white, far = black, range found automatically
    Normal,   // -1..+1 directions shown as normal-map colours
    Vector,   // motion vectors, positions ...: scaled to fit around mid-grey
};

struct ViewOptions {
    bool invertDepth = false;   // near = black, far = white
};

struct ExrLayer {
    std::string name;        // e.g. "" (main image), "diffuse", "left/beauty"
    std::string channels;    // what it shows, e.g. "RGBA", "Y", "XYZ", "Z"
    int part = 0;
    int src[3] = {-1, -1, -1};   // channel used for R, G, B (-1 = black)
    int width = 0, height = 0;
    int crypto = -1;             // >= 0: this layer is a Cryptomatte (index into the document's sets)
    LayerView view = LayerView::Color;
};

// One Cryptomatte (e.g. CryptoObject, CryptoMaterial): its rank channels are
// pairs of (object ID, coverage), most-covering first.
struct CryptoSet {
    std::string name;
    int part = 0;
    std::vector<std::pair<int, int>> ranks;               // (id channel, coverage channel)
    std::unordered_map<uint32_t, std::string> names;      // from the manifest, if the file has one
};

// An object found at a pixel: its ID and how much of the pixel it covers.
struct CryptoHit {
    uint32_t id = 0;
    float coverage = 0.0f;
};

class ExrDocument {
public:
    // Decodes everything in the file (half-float channels stay half-float to
    // save memory). Returns null and fills `error` on failure.
    static std::shared_ptr<ExrDocument> Load(const std::vector<unsigned char>& data, std::string& error);
    ~ExrDocument();
    ExrDocument(const ExrDocument&) = delete;
    ExrDocument& operator=(const ExrDocument&) = delete;

    const std::vector<ExrLayer>& Layers() const { return layers_; }
    int DefaultLayer() const { return defaultLayer_; }
    int PartCount() const { return static_cast<int>(parts_.size()); }

    // Makes the image for layer `index`: linear RGB for colour passes, a
    // display-referred view for data passes and Cryptomattes.
    bool BuildLayer(int index, HdrImage& out, std::string& error, const ViewOptions& opts = {}) const;

    // The stored values of a layer's channels at (x, y), before any view.
    void RawAt(int index, int x, int y, float out[3]) const;

    // "depth view" etc., or empty for colour passes.
    static const char* ViewName(LayerView v);

    // Display name for a layer ("Main image" for the unnamed one).
    static std::string DisplayName(const ExrLayer& l);

    // --- Cryptomatte ---
    int CryptoCount() const { return static_cast<int>(cryptos_.size()); }
    const CryptoSet& Crypto(int i) const { return cryptos_[static_cast<size_t>(i)]; }

    // Objects covering pixel (x, y), most coverage first.
    std::vector<CryptoHit> CryptoAt(int set, int x, int y) const;

    // Name from the manifest, or the ID in hex when there's no name.
    std::string CryptoName(int set, uint32_t id) const;

    // The ID colour view (`isolate` empty), or a white mask of the objects
    // in `isolate`. The result is display-referred (no tonemap).
    bool BuildCryptoView(int set, const std::vector<uint32_t>& isolate, HdrImage& out, std::string& error) const;

private:
    ExrDocument() = default;
    struct Part;
    struct CryptoMeta { std::string name, manifest; };
    void FindLayers(const std::vector<std::vector<CryptoMeta>>& meta);
    void ConvertChannel(const Part& p, int ch, float* out, int stride, int offset) const;
    float SampleRaw(const Part& p, int ch, int x, int y) const;

    std::vector<std::unique_ptr<Part>> parts_;
    std::vector<ExrLayer> layers_;
    std::vector<CryptoSet> cryptos_;
    int defaultLayer_ = 0;
};
