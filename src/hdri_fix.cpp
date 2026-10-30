// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hdri_fix.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

#include "parallel.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr float kMaxSunSizeDeg = 12.0f;                            // bigger clipped areas aren't a sun
constexpr float kClipTolerance = 0.003f;                           // values within 0.3% count as one flat level
constexpr float kMinClipOverMedian = 8.0f;                         // clipping levels are at least this x the median
constexpr size_t kManyHotPixels = 500;                             // more than this: probably stars / lights

inline float Lum(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }
inline bool Finite3(const float* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }

// Equirectangular geometry. Continuous image coordinates: pixel (x, y) covers [x, x+1) x [y, y+1).
struct Geo {
    int w = 0, h = 0;
    double Lat(double y) const { return kPi * (0.5 - y / h); }
    double Lon(double x) const { return 2.0 * kPi * x / w - kPi; }
    double PixelOmega(int y) const { return (2.0 * kPi / w) * (kPi / h) * std::cos(Lat(y + 0.5)); }
    void Dir(double x, double y, double d[3]) const {
        const double la = Lat(y), lo = Lon(x);
        d[0] = std::cos(la) * std::sin(lo);
        d[1] = std::sin(la);
        d[2] = std::cos(la) * std::cos(lo);
    }
    // Continuous image position of a direction.
    void Pos(const double d[3], double& x, double& y) const {
        const double la = std::asin(std::clamp(d[1], -1.0, 1.0));
        const double lo = std::atan2(d[0], d[2]);
        x = (lo + kPi) / (2.0 * kPi) * w;
        y = (0.5 - la / kPi) * h;
    }
};

// Areas left alone by the hot-pixel search (the sun and the brightest pixel:
// a tiny unclipped sun in a small HDRI can be a single pixel).
struct Zone {
    double d[3] = {0, 0, 0};
    double cosLimit = 2.0;   // panoramas: inside when dot >= cosLimit
    int cx = 0, cy = 0, radius = 0;   // flat images: inside when within `radius` pixels
};

bool InZones(const std::vector<Zone>& zones, const Geo& geo, bool pano, int x, int y) {
    if (zones.empty()) return false;
    double d[3];
    if (pano) geo.Dir(x + 0.5, y + 0.5, d);
    for (const Zone& z : zones) {
        if (pano) {
            if (d[0] * z.d[0] + d[1] * z.d[1] + d[2] * z.d[2] >= z.cosLimit) return true;
        } else {
            const int dx = x - z.cx, dy = y - z.cy;
            if (dx * dx + dy * dy <= z.radius * z.radius) return true;
        }
    }
    return false;
}

// Index of neighbour (x+dx, y+dy), wrapping left/right for panoramas; -1 outside.
inline long long Neighbour(int x, int y, int dx, int dy, int w, int h, bool pano) {
    int nx = x + dx, ny = y + dy;
    if (ny < 0 || ny >= h) return -1;
    if (nx < 0 || nx >= w) {
        if (!pano) return -1;
        nx = (nx + w) % w;
    }
    return static_cast<long long>(ny) * w + nx;
}

// Single pixels far brighter than all 8 neighbours and than the scene.
std::vector<size_t> FindHotPixels(const float* px, int w, int h, bool pano, float median,
                                  const std::vector<Zone>& zones) {
    const Geo geo{w, h};
    const float minLum = std::max(100.0f * median, 1e-4f);
    std::vector<size_t> found;
    std::mutex m;
    ParallelRows(h, [&](int y0, int y1) {
        std::vector<size_t> local;
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* p = px + (static_cast<size_t>(y) * w + x) * 3;
                if (!Finite3(p)) continue;
                const float L = Lum(p);
                if (!(L > minLum)) continue;
                float nmax = 0.0f;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const long long ni = Neighbour(x, y, dx, dy, w, h, pano);
                        if (ni < 0) continue;
                        const float* q = px + static_cast<size_t>(ni) * 3;
                        if (!Finite3(q)) continue;
                        nmax = std::max(nmax, Lum(q));
                        ++count;
                    }
                if (count < 3 || !(L > 20.0f * nmax)) continue;
                if (InZones(zones, geo, pano, x, y)) continue;
                local.push_back(static_cast<size_t>(y) * w + x);
            }
        }
        std::lock_guard<std::mutex> lock(m);
        found.insert(found.end(), local.begin(), local.end());
    }, w * 4LL);
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<Zone> ProtectedZones(const HdriReport& r, const float* px) {
    std::vector<Zone> zones;
    const Geo geo{r.width, r.height};
    auto add = [&](double x, double y, double radiusDeg) {
        Zone z;
        geo.Dir(x, y, z.d);
        z.cosLimit = std::cos(radiusDeg * kDeg);
        z.cx = static_cast<int>(x);
        z.cy = static_cast<int>(y);
        z.radius = std::max(3, r.width / 50);
        zones.push_back(z);
    };
    // The brightest pixel, when its surroundings glow (an unclipped sun or a
    // lamp); a lone bright pixel on a dull background is a hot pixel.
    {
        double sum = 0;
        int cnt = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                if (std::abs(dx) < 2 && std::abs(dy) < 2) continue;   // the outer ring of a 5x5 block
                const long long ni = Neighbour(r.maxX, r.maxY, dx, dy, r.width, r.height, r.panorama);
                if (ni < 0 || !Finite3(px + static_cast<size_t>(ni) * 3)) continue;
                sum += std::max(0.0f, Lum(px + static_cast<size_t>(ni) * 3));
                ++cnt;
            }
        if (cnt && sum / cnt > 3.0 * std::max(r.medianLum, 1e-6f)) add(r.maxX + 0.5, r.maxY + 0.5, 3.0);
    }
    if (r.sunSizeDeg > 0.0f) add(r.sunX, r.sunY, std::max(3.0, static_cast<double>(r.sunSizeDeg)));
    return zones;
}

// A connected area of flat pixels. Its members are order[start .. start+count).
struct Area {
    size_t start = 0, count = 0, interior = 0;
    double omega = 0.0, d[3] = {0, 0, 0}, lumSum = 0.0, energy = 0.0;
};

long long SortedIndex(const std::vector<size_t>& v, long long idx) {
    if (idx < 0) return -1;
    auto it = std::lower_bound(v.begin(), v.end(), static_cast<size_t>(idx));
    return (it != v.end() && *it == static_cast<size_t>(idx)) ? it - v.begin() : -1;
}

// Pixels with any channel c inside [lo[c], hi[c]], in ascending order.
// False when there are more than `cap` (then `out` is incomplete).
bool CollectBand(const float* px, int w, int h, const float lo[3], const float hi[3], size_t cap,
                 std::vector<size_t>& out) {
    out.clear();
    std::atomic<size_t> total{0};
    std::mutex m;
    ParallelRows(h, [&](int y0, int y1) {
        std::vector<size_t> local;
        for (size_t i = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w; i < e; ++i) {
            const float* p = px + i * 3;
            if (!Finite3(p)) continue;
            if ((p[0] >= lo[0] && p[0] <= hi[0]) || (p[1] >= lo[1] && p[1] <= hi[1]) || (p[2] >= lo[2] && p[2] <= hi[2])) {
                if (total.fetch_add(1) >= cap) break;
                local.push_back(i);
            }
        }
        std::lock_guard<std::mutex> lock(m);
        out.insert(out.end(), local.begin(), local.end());
    }, w * 2LL);
    std::sort(out.begin(), out.end());
    return total.load() <= cap;
}

// Groups the pixels in `clip` (sorted) into connected areas and keeps the
// ones that are real flat tops: at least 3 connected pixels at one level
// (a small group (a clipped sun in a small
// image is only a few pixels; an unclipped peak rarely repeats its top value).
void FindAreas(const float* px, int w, int h, bool pano, const std::vector<size_t>& clip,
               std::vector<size_t>& order, std::vector<Area>& areas) {
    const Geo geo{w, h};
    order.clear();
    areas.clear();
    std::vector<unsigned char> seen(clip.size(), 0);
    order.reserve(clip.size());
    for (size_t s = 0; s < clip.size(); ++s) {
        if (seen[s]) continue;
        Area a;
        a.start = order.size();
        seen[s] = 1;
        order.push_back(s);
        float areaMax[3] = {0, 0, 0}, outsideMax[3] = {0, 0, 0};
        for (size_t q = a.start; q < order.size(); ++q) {
            const size_t idx = clip[order[q]];
            const int x = static_cast<int>(idx % w), y = static_cast<int>(idx / w);
            int four = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const long long ni = Neighbour(x, y, dx, dy, w, h, pano);
                    const long long k = SortedIndex(clip, ni);
                    if (k < 0) {
                        if (ni >= 0) {   // just outside the area
                            const float* q2 = px + static_cast<size_t>(ni) * 3;
                            if (Finite3(q2)) for (int c = 0; c < 3; ++c) outsideMax[c] = std::max(outsideMax[c], q2[c]);
                        }
                        continue;
                    }
                    if (!dx || !dy) ++four;
                    if (!seen[static_cast<size_t>(k)]) { seen[static_cast<size_t>(k)] = 1; order.push_back(static_cast<size_t>(k)); }
                }
            if (four == 4) ++a.interior;
            const float* p = px + idx * 3;
            for (int c = 0; c < 3; ++c) areaMax[c] = std::max(areaMax[c], p[c]);
            const double om = pano ? geo.PixelOmega(y) : 1.0;
            double d[3];
            geo.Dir(x + 0.5, y + 0.5, d);
            for (int c = 0; c < 3; ++c) a.d[c] += d[c] * om;
            a.omega += om;
            a.lumSum += Lum(p);
            a.energy += Lum(p) * om;
        }
        a.count = order.size() - a.start;
        // A clipped plateau is the top: nothing right next to it is brighter.
        // Equal values on the slope of a real (unclipped) peak have brighter
        // pixels beside them, so they are not clipping.
        bool isTop = true;
        for (int c = 0; c < 3; ++c)
            if (outsideMax[c] > areaMax[c] * 1.01f) isTop = false;
        if (a.count >= 3 && isTop) areas.push_back(a);
    }
}

// Values of channel c shared by at least 5 pixels (within the tolerance) and
// well above the median (`minLevel`),
// brightest first: the candidates for a clipping level. Looks only at the
// brightest pixels of the channel (found with a log histogram).
std::vector<float> TopLevels(const float* px, int w, int h, int c, float cmax, float minLevel) {
    std::vector<float> levels;
    if (!(cmax > 0.0f) || cmax < minLevel) return levels;
    const size_t n = static_cast<size_t>(w) * h;
    constexpr int B = 2048;
    constexpr double kStops = 24.0;
    const double top = std::log2(static_cast<double>(cmax)), bottom = top - kStops;
    auto binOf = [&](float v) {
        const int b = static_cast<int>((std::log2(static_cast<double>(v)) - bottom) / kStops * B);
        return std::clamp(b, 0, B - 1);
    };
    std::vector<size_t> hist(B, 0);
    std::mutex m;
    ParallelRows(h, [&](int y0, int y1) {
        std::vector<size_t> local(B, 0);
        for (size_t i = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w; i < e; ++i) {
            const float v = px[i * 3 + c];
            if (v > 0.0f && std::isfinite(v)) ++local[static_cast<size_t>(binOf(v))];
        }
        std::lock_guard<std::mutex> lock(m);
        for (int b = 0; b < B; ++b) hist[static_cast<size_t>(b)] += local[static_cast<size_t>(b)];
    }, w);

    // Take whole bins from the top while they hold at most `keep` values.
    const size_t keep = std::max<size_t>(20000, n / 500);
    if (hist[B - 1] > keep) { levels.push_back(cmax); return levels; }   // a huge flat top
    int lowest = B - 1;
    size_t acc = hist[B - 1];
    while (lowest > 0 && acc + hist[static_cast<size_t>(lowest - 1)] <= keep) acc += hist[static_cast<size_t>(--lowest)];
    const float threshold = lowest == 0 ? 0.0f : static_cast<float>(std::exp2(bottom + lowest * kStops / B));

    std::vector<float> vals;
    vals.reserve(acc + 16);
    for (size_t i = 0; i < n; ++i) {
        const float v = px[i * 3 + c];
        if (v >= threshold && v > 0.0f && std::isfinite(v)) vals.push_back(v);
    }
    std::sort(vals.begin(), vals.end(), std::greater<float>());
    for (size_t i = 0; i < vals.size() && levels.size() < 4;) {
        const float v0 = vals[i];
        if (v0 < minLevel) break;
        size_t j = i;
        while (j < vals.size() && vals[j] >= v0 * (1.0f - kClipTolerance)) ++j;
        if (j - i >= 5) levels.push_back(vals[i + (j - i) / 2]);
        i = j;
    }
    return levels;
}

// Clear-sky ratio of direct sun (on a surface facing it) to sky light (on a
// flat surface), by the sun's height: about 1.2 at 3 deg, 6.3 at 19 deg,
// 11 at 40 deg, 14 at 60 deg. Fitted to real, unclipped clear-sky HDRIs
// (tests/light_test.cpp: clip a copy, fix it, compare with the original).
double SunToSkyRatio(double elevationDeg) {
    const double h = std::max(1.0, elevationDeg);
    return 20.0 * (1.0 - std::exp(-h / 50.0));
}

}  // namespace

HdriReport AnalyzeHdri(const HdrImage& img) {
    HdriReport r;
    if (img.empty()) return r;
    const int w = img.width, h = img.height;
    const size_t n = static_cast<size_t>(w) * h;
    const float* px = img.rgb();
    r.width = w;
    r.height = h;
    const double aspect = static_cast<double>(w) / h;
    r.panorama = aspect > 1.9 && aspect < 2.1;
    const bool pano = r.panorama;
    const Geo geo{w, h};

    // 1. Broken values, brightest values.
    float cmax[3] = {0, 0, 0};
    float maxLum = -1.0f;
    size_t maxIdx = 0;
    std::mutex m;
    ParallelRows(h, [&](int y0, int y1) {
        size_t nan = 0, inf = 0, neg = 0, li = 0;
        float lc[3] = {0, 0, 0}, ll = -1.0f;
        for (size_t i = static_cast<size_t>(y0) * w, e = static_cast<size_t>(y1) * w; i < e; ++i) {
            const float* p = px + i * 3;
            bool isNan = false, isInf = false, isNeg = false;
            for (int c = 0; c < 3; ++c) {
                const float v = p[c];
                if (std::isnan(v)) isNan = true;
                else if (std::isinf(v)) isInf = true;
                else {
                    if (v < 0.0f) isNeg = true;
                    if (v > lc[c]) lc[c] = v;
                }
            }
            nan += isNan; inf += isInf; neg += isNeg;
            if (!isNan && !isInf) {
                const float L = Lum(p);
                if (L > ll) { ll = L; li = i; }
            }
        }
        std::lock_guard<std::mutex> lock(m);
        r.nanPixels += nan; r.infPixels += inf; r.negativePixels += neg;
        for (int c = 0; c < 3; ++c) cmax[c] = std::max(cmax[c], lc[c]);
        if (ll > maxLum) { maxLum = ll; maxIdx = li; }
    }, w * 3LL);
    r.maxValue = std::max({cmax[0], cmax[1], cmax[2]});
    r.maxLum = std::max(0.0f, maxLum);
    r.maxX = static_cast<int>(maxIdx % w);
    r.maxY = static_cast<int>(maxIdx / w);

    // 2. Median brightness (from up to ~1M samples) and dynamic range.
    {
        const size_t step = std::max<size_t>(1, n / 1000000);
        std::vector<float> s;
        s.reserve(n / step + 1);
        for (size_t i = 0; i < n; i += step) {
            const float* p = px + i * 3;
            if (Finite3(p)) s.push_back(std::max(0.0f, Lum(p)));
        }
        if (!s.empty()) {
            std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
            r.medianLum = s[s.size() / 2];
        }
        const float base = std::max({r.medianLum, r.maxLum * 1e-9f, 1e-9f});
        r.rangeStops = r.maxLum > 0.0f ? static_cast<float>(std::log2(r.maxLum / base)) : 0.0f;
    }
    r.notHdr = r.maxValue > 0.0f && r.maxValue <= 1.001f;

    // 3. Clipping: flat plateaus near the top of each channel. Not just at the
    //    single brightest value: one hot pixel or a specular glint above the
    //    clipped sun must not hide it.
    std::vector<size_t> clip, order;
    std::vector<Area> areas;
    if (!r.notHdr && r.maxValue > 0.0f) {
        const size_t cap = std::max<size_t>(n / 20, 1000);
        // Clipping happens at bright values: well above the typical brightness
        // (a smooth bright sky can repeat values without being clipped).
        const float minLevel = kMinClipOverMedian * r.medianLum;
        float lo[3], hi[3];
        bool any = false, overflow = false;
        for (int c = 0; c < 3; ++c) {
            lo[c] = INFINITY; hi[c] = -INFINITY;
            for (float level : TopLevels(px, w, h, c, cmax[c], minLevel)) {
                float l1[3] = {INFINITY, INFINITY, INFINITY}, h1[3] = {-INFINITY, -INFINITY, -INFINITY};
                l1[c] = level * (1.0f - kClipTolerance);
                h1[c] = level * (1.0f + kClipTolerance);
                std::vector<size_t> band, ord;
                std::vector<Area> found;
                if (!CollectBand(px, w, h, l1, h1, cap, band)) { overflow = true; lo[c] = l1[c]; hi[c] = h1[c]; any = true; break; }
                FindAreas(px, w, h, pano, band, ord, found);
                if (!found.empty()) { lo[c] = l1[c]; hi[c] = h1[c]; any = true; break; }
            }
        }
        if (any && (overflow || !CollectBand(px, w, h, lo, hi, cap, clip))) {
            // A huge part of the image is flat white: badly overexposed, not a sun.
            r.clipped = true;
            r.clippedAreas = 1;
            r.clipLevel = r.maxLum;
            r.clippedPixels = cap;
            r.sun = pano ? HdriReport::Sun::TooLarge : HdriReport::Sun::NotPanorama;
            clip.clear();
        } else if (any) {
            FindAreas(px, w, h, pano, clip, order, areas);
        }
    }
    auto clipIndex = [&](long long idx) { return SortedIndex(clip, idx); };

    if (!areas.empty()) {
        r.clipped = true;
        r.clippedAreas = static_cast<int>(areas.size());
        const Area* biggest = &areas[0];
        for (const Area& a : areas) if (a.omega > biggest->omega) biggest = &a;
        r.clipLevel = static_cast<float>(biggest->lumSum / biggest->count);
        r.clippedPixels = biggest->count;

        if (!pano) {
            r.sun = HdriReport::Sun::NotPanorama;
        } else {
            // The sun: the largest flat area whose centre is above the horizon.
            const Area* sun = nullptr;
            double sx = 0, sy = 0, elev = 0;
            for (const Area& a : areas) {
                double d[3] = {a.d[0], a.d[1], a.d[2]};
                const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len <= 0) continue;
                for (double& v : d) v /= len;
                const double e = std::asin(std::clamp(d[1], -1.0, 1.0)) / kDeg;
                if (e < -2.0) continue;
                if (!sun || a.omega > sun->omega) { sun = &a; geo.Pos(d, sx, sy); elev = e; }
            }
            if (!sun) {
                r.sun = HdriReport::Sun::BelowHorizon;
            } else {
                r.sunX = static_cast<float>(sx);
                r.sunY = static_cast<float>(sy);
                r.sunElevation = static_cast<float>(elev);
                r.sunSizeDeg = static_cast<float>(2.0 * std::sqrt(sun->omega / kPi) / kDeg);
                r.clipLevel = static_cast<float>(sun->lumSum / sun->count);
                r.clippedPixels = sun->count;
                r.sun = r.sunSizeDeg > kMaxSunSizeDeg ? HdriReport::Sun::TooLarge : HdriReport::Sun::Rebuildable;
                r.wideGlow = r.sunSizeDeg > kWideGlowDeg;

                if (r.sun == HdriReport::Sun::Rebuildable) {
                    // Colour of the glow just outside the flat area (1 to 3 pixels away).
                    std::unordered_set<size_t> ring;
                    double rgb[3] = {0, 0, 0};
                    for (size_t q = sun->start; q < sun->start + sun->count; ++q) {
                        const size_t idx = clip[order[q]];
                        const int x = static_cast<int>(idx % w), y = static_cast<int>(idx / w);
                        for (int dy = -3; dy <= 3; ++dy)
                            for (int dx = -3; dx <= 3; ++dx) {
                                const long long ni = Neighbour(x, y, dx, dy, w, h, pano);
                                if (ni < 0 || clipIndex(ni) >= 0) continue;
                                if (!ring.insert(static_cast<size_t>(ni)).second) continue;
                                const float* p = px + static_cast<size_t>(ni) * 3;
                                if (!Finite3(p)) continue;
                                for (int c = 0; c < 3; ++c) rgb[c] += std::max(0.0f, p[c]);
                            }
                    }
                    const double L = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
                    if (L > 0) {
                        float col[3];
                        for (int c = 0; c < 3; ++c) col[c] = std::clamp(static_cast<float>(rgb[c] / L), 0.6f, 1.4f);
                        const float cl = Lum(col);
                        for (int c = 0; c < 3; ++c) r.sunColor[c] = col[c] / cl;
                        const float hi = std::max({r.sunColor[0], r.sunColor[1], r.sunColor[2]});
                        const float lo = std::min({r.sunColor[0], r.sunColor[1], r.sunColor[2]});
                        r.glowNeutral = hi < lo * 1.08f;
                    }

                    // Sky light on a flat surface, leaving out the sun itself: the
                    // clipped area and everything within 1 deg of its centre (an
                    // unclipped sun disc must not count as sky).
                    double sc[3];
                    geo.Dir(sx, sy, sc);
                    const double exclude = std::max(1.0, 0.5 * r.sunSizeDeg + 0.5) * kDeg;
                    const double cosEx = std::cos(exclude);
                    const double latSun = std::asin(std::clamp(sc[1], -1.0, 1.0));
                    double sky = 0.0;
                    ParallelRows(h / 2, [&](int y0, int y1) {
                        double local = 0.0;
                        for (int y = y0; y < y1; ++y) {
                            const double lat = geo.Lat(y + 0.5);
                            if (lat <= 0) continue;
                            const bool nearSun = std::fabs(lat - latSun) <= exclude + kPi / h;
                            const double wgt = std::sin(lat) * geo.PixelOmega(y);
                            double row = 0.0;
                            for (int x = 0; x < w; ++x) {
                                const float* p = px + (static_cast<size_t>(y) * w + x) * 3;
                                if (!Finite3(p)) continue;
                                if (nearSun) {
                                    double d[3];
                                    geo.Dir(x + 0.5, y + 0.5, d);
                                    if (d[0] * sc[0] + d[1] * sc[1] + d[2] * sc[2] >= cosEx) continue;
                                }
                                row += std::max(0.0f, Lum(p));
                            }
                            local += row * wgt;
                        }
                        std::lock_guard<std::mutex> lock(m);
                        sky += local;
                    }, w * 2LL);
                    for (size_t q = sun->start; q < sun->start + sun->count; ++q) {   // flat pixels outside that circle
                        const size_t idx = clip[order[q]];
                        const int x = static_cast<int>(idx % w), y = static_cast<int>(idx / w);
                        const double lat = geo.Lat(y + 0.5);
                        double d[3];
                        geo.Dir(x + 0.5, y + 0.5, d);
                        if (lat > 0 && d[0] * sc[0] + d[1] * sc[1] + d[2] * sc[2] < cosEx)
                            sky -= Lum(px + idx * 3) * std::sin(lat) * geo.PixelOmega(y);
                    }
                    r.skyIrradiance = std::max(0.0, sky);
                    // Never less than what the flat area already holds.
                    r.sunIrradiance = std::max(SunToSkyRatio(elev) * r.skyIrradiance, sun->energy);
                    r.clippedEnergy = sun->energy;
                    r.lightShare = r.sunIrradiance > 0 ? static_cast<float>(std::min(1.0, sun->energy / r.sunIrradiance)) : 1.0f;
                    r.sunPeak = SunPeakAt(r, 0.0f);
                    // Already about as bright as a real sun would be: a flat top
                    // from the file's precision, not clipping. Leave it alone.
                    if (r.sunPeak < 2.0f * r.clipLevel) r.sun = HdriReport::Sun::LooksOk;
                }
            }
        }
    }

    // 4. Hot pixels (counted now, removed by FixHdri).
    r.hotPixels = FindHotPixels(px, w, h, pano, r.medianLum, ProtectedZones(r, px)).size();
    return r;
}

float SkyEV(SkyType sky) {
    switch (sky) {
        case SkyType::LightHaze: return -1.0f;
        case SkyType::Hazy: return -1.5f;
        default: return 0.0f;
    }
}

float SunPeakAt(const HdriReport& r, float strengthEV, float sizeDeg) {
    const double e = r.sunIrradiance * std::exp2(static_cast<double>(strengthEV));
    const double radius = 0.5 * std::max(0.1f, sizeDeg) * kDeg;
    return std::max(r.clipLevel, static_cast<float>(e / (kPi * radius * radius)));
}

// Correlated colour temperature of direct sunlight by its height (approximate
// clear-sky values), turned into linear sRGB through the Planckian locus.
void NaturalSunColor(float elevationDeg, float out[3]) {
    static const float table[][2] = {{0, 2000}, {2, 2500}, {5, 3000}, {10, 3800}, {20, 4700},
                                     {30, 5200}, {45, 5600}, {60, 5800}, {90, 5900}};
    const float h = std::clamp(elevationDeg, 0.0f, 90.0f);
    double T = 5900;
    for (size_t i = 0; i + 1 < std::size(table); ++i)
        if (h <= table[i + 1][0]) {
            const float t = (h - table[i][0]) / (table[i + 1][0] - table[i][0]);
            T = table[i][1] + t * (table[i + 1][1] - table[i][1]);
            break;
        }
    // Kim et al. cubic fit of the Planckian locus (1667 K .. 25000 K).
    double x, y;
    if (T <= 4000) x = -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910;
    else x = -3.0258469e9 / (T * T * T) + 2.1070379e6 / (T * T) + 0.2226347e3 / T + 0.240390;
    if (T <= 2222) y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    else if (T <= 4000) y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    else y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    double rgb[3] = {3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
                     0.0557 * X - 0.2040 * Y + 1.0570 * Z};
    for (double& v : rgb) v = std::max(0.0, v);
    const double L = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
    for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(rgb[c] / L);
}

bool HdriManyHotPixels(const HdriReport& r) { return r.hotPixels > kManyHotPixels; }

bool FixHdri(const HdrImage& src, const HdriReport& report, const HdriFixOptions& opts,
             HdrImage& out, HdriFixResult& result, std::string& error) {
    result = HdriFixResult{};
    if (src.empty()) { error = "There is no image to fix."; return false; }
    const int w = src.width, h = src.height;
    const size_t n = static_cast<size_t>(w) * h;
    const bool pano = report.panorama;
    const Geo geo{w, h};

    float* dst = static_cast<float*>(std::malloc(n * 3 * sizeof(float)));
    if (!dst) { error = "Not enough memory to fix an image this large."; return false; }
    out.clear();
    out.width = w;
    out.height = h;
    out.format = src.format;
    out.pixels.reset(dst);
    const float* sp = src.rgb();
    std::memcpy(dst, sp, n * 3 * sizeof(float));

    try {
        // 1. Broken values: NaN / infinite channels get the average of their
        //    usable neighbours (or 0), negative values become 0.
        if (opts.repairBroken && (report.nanPixels || report.infPixels || report.negativePixels)) {
            std::atomic<size_t> fixedCount{0};
            ParallelRows(h, [&](int y0, int y1) {
                size_t local = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = static_cast<size_t>(y) * w + x;
                        float* p = dst + i * 3;
                        bool changed = false;
                        for (int c = 0; c < 3; ++c) {
                            if (!std::isfinite(sp[i * 3 + c])) {
                                double sum = 0;
                                int cnt = 0;
                                for (int dy = -1; dy <= 1; ++dy)
                                    for (int dx = -1; dx <= 1; ++dx) {
                                        const long long ni = Neighbour(x, y, dx, dy, w, h, pano);
                                        if (ni < 0 || (!dx && !dy)) continue;
                                        const float v = sp[static_cast<size_t>(ni) * 3 + c];
                                        if (std::isfinite(v)) { sum += std::max(0.0f, v); ++cnt; }
                                    }
                                p[c] = cnt ? static_cast<float>(sum / cnt) : 0.0f;
                                changed = true;
                            }
                            if (p[c] < 0.0f) { p[c] = 0.0f; changed = true; }
                        }
                        local += changed;
                    }
                fixedCount += local;
            }, w * 3LL);
            result.repaired = fixedCount.load();
        }

        // 2. Hot pixels: replaced by the median of their neighbours.
        if (opts.removeHotPixels && report.hotPixels) {
            const std::vector<size_t> hot = FindHotPixels(dst, w, h, pano, report.medianLum, ProtectedZones(report, sp));
            for (size_t idx : hot) {
                const int x = static_cast<int>(idx % w), y = static_cast<int>(idx / w);
                for (int c = 0; c < 3; ++c) {
                    float v[8];
                    int cnt = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const long long ni = Neighbour(x, y, dx, dy, w, h, pano);
                            if (ni < 0 || (!dx && !dy)) continue;
                            const float q = dst[static_cast<size_t>(ni) * 3 + c];
                            if (std::isfinite(q)) v[cnt++] = q;
                        }
                    if (!cnt) continue;
                    std::nth_element(v, v + cnt / 2, v + cnt);
                    dst[idx * 3 + c] = v[cnt / 2];
                }
            }
            result.hotRemoved = hot.size();
        }

        // 3. The sun: a disc of the real angular size, its brightness set so
        //    the light it gives matches the estimate (energy per solid angle,
        //    so it is right at any resolution, even when the disc is smaller
        //    than a pixel). Edge pixels are blended by how much they cover.
        if (opts.rebuildSun && report.sun == HdriReport::Sun::Rebuildable && pano) {
            double c0[3];
            geo.Dir(report.sunX, report.sunY, c0);
            const double sunRadius = 0.5 * std::max(0.1f, opts.sunSizeDeg) * kDeg;
            const double cosR = std::cos(sunRadius);
            const double pxAng = kPi / h;
            const double margin = sunRadius + 2.0 * pxAng;
            float color[3] = {report.sunColor[0], report.sunColor[1], report.sunColor[2]};
            if (opts.sunColor == SunColorMode::Natural) NaturalSunColor(report.sunElevation, color);
            const double latc = geo.Lat(report.sunY);
            const int yTop = std::clamp(static_cast<int>(std::floor((0.5 - (latc + margin) / kPi) * h)) - 1, 0, h - 1);
            const int yBot = std::clamp(static_cast<int>(std::ceil((0.5 - (latc - margin) / kPi) * h)) + 1, 0, h - 1);
            constexpr int S = 8;   // sub-samples per pixel side

            struct Hit { size_t idx; double cover, omega; };
            std::vector<Hit> hits;
            for (int y = yTop; y <= yBot; ++y) {
                const double cl = std::cos(geo.Lat(y + 0.5));
                int x0, x1;
                if (cl < 1e-6 || margin / cl >= kPi) { x0 = 0; x1 = w - 1; }
                else {
                    const double span = (margin / cl) / (2.0 * kPi) * w + 2.0;
                    x0 = static_cast<int>(std::floor(report.sunX - span));
                    x1 = static_cast<int>(std::ceil(report.sunX + span));
                    if (x1 - x0 >= w) { x0 = 0; x1 = w - 1; }
                }
                for (int xx = x0; xx <= x1; ++xx) {
                    const int x = ((xx % w) + w) % w;
                    int in = 0;
                    for (int sy = 0; sy < S; ++sy)
                        for (int sx = 0; sx < S; ++sx) {
                            double d[3];
                            geo.Dir(x + (sx + 0.5) / S, y + (sy + 0.5) / S, d);
                            if (d[0] * c0[0] + d[1] * c0[1] + d[2] * c0[2] >= cosR) ++in;
                        }
                    if (in) hits.push_back({static_cast<size_t>(y) * w + x, in / double(S * S), geo.PixelOmega(y)});
                }
            }
            if (hits.empty()) {   // a tiny image: the disc falls between sub-samples
                const int x = std::clamp(static_cast<int>(report.sunX), 0, w - 1);
                const int y = std::clamp(static_cast<int>(report.sunY), 0, h - 1);
                hits.push_back({static_cast<size_t>(y) * w + x, 1.0, geo.PixelOmega(y)});
            }
            double area = 0.0;
            for (const Hit& t : hits) area += t.cover * t.omega;
            const double target = report.sunIrradiance * std::exp2(static_cast<double>(SkyEV(opts.sky) + opts.sunStrengthEV));
            const float peak = std::max(report.clipLevel, static_cast<float>(target / std::max(area, 1e-12)));
            for (const Hit& t : hits) {
                float* p = dst + t.idx * 3;
                const float c = static_cast<float>(t.cover);
                for (int k = 0; k < 3; ++k) p[k] = p[k] * (1.0f - c) + color[k] * peak * c;
            }
            result.sunRebuilt = true;
            result.sunPeak = peak;
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        error = "Not enough memory to fix an image this large.";
        return false;
    }
    return true;
}

HdriFixOptions DefaultFixOptions(const HdriReport& report) {
    HdriFixOptions o;
    // A wide clipped glow (hazy or sunset sun) is shown but not ticked: a sharp sun there adds far too much light.
    o.rebuildSun = report.sun == HdriReport::Sun::Rebuildable && !report.wideGlow;
    o.repairBroken = report.nanPixels || report.infPixels || report.negativePixels;
    o.removeHotPixels = report.hotPixels > 0 && !HdriManyHotPixels(report);
    o.sky = SkyType::Clear;
    o.sunStrengthEV = 0.0f;
    // The glow's own colour matched the real sun in the tests; "Natural" is a choice, not the default.
    o.sunColor = SunColorMode::Glow;
    return o;
}
