// EXRay (HDR & EXR viewer) — test tool
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Lighting test for Fix HDRI: renders the same scene lit by an HDRI before and
// after the fix. Matte grey ball, glossy ball, white cube on a grey floor.
// The HDRI is the only light; the sun gets no special treatment, so whatever
// difference shows up comes from the pixels of the HDRI alone.
//
//   light_test <in.hdr> <out_prefix> [strengthEV]
//
// Writes <prefix>_original.png, <prefix>_fixed.png and prints shadow numbers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hdri_fix.h"
#include "image_io.h"
#include "parallel.h"
#include "stb_image_write.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

struct V { double x, y, z; };
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V norm(V a) { double l = std::sqrt(dot(a, a)); return a * (1.0 / l); }

// Environment lookup: direction -> equirect pixel, with a rotation (yaw) so the
// sun sits behind-left of the objects as seen from the camera.
struct Env {
    const HdrImage* img;
    double yaw;   // radians added to the longitude
    V Look(V d) const {
        const double lat = std::asin(std::clamp(d.y, -1.0, 1.0));
        double lon = std::atan2(d.x, d.z) + yaw;
        lon = std::fmod(lon + 3 * kPi, 2 * kPi) - kPi;
        int x = static_cast<int>((lon + kPi) / (2 * kPi) * img->width);
        int y = static_cast<int>((0.5 - lat / kPi) * img->height);
        x = std::clamp(x, 0, img->width - 1); y = std::clamp(y, 0, img->height - 1);
        const float* p = img->rgb() + (static_cast<size_t>(y) * img->width + x) * 3;
        return {p[0], p[1], p[2]};
    }
};

// The HDRI as a list of small lights (energy-preserving): the brightest cells
// of a 1024x512 grid on their own, everything else merged into 64x32 cells.
struct Light { V dir; V e; };   // e = radiance x solid angle (irradiance facing it)

std::vector<Light> MakeLights(const HdrImage& img, double yaw) {
    const int W = 1024, H = 512, k = img.width / W;
    std::vector<V> cell(static_cast<size_t>(W) * H, V{0, 0, 0});
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            V s{0, 0, 0};
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    const float* p = img.rgb() + (static_cast<size_t>(y * k + dy) * img.width + x * k + dx) * 3;
                    s = s + V{p[0], p[1], p[2]};
                }
            const double lat = kPi * (0.5 - (y + 0.5) / H);
            const double om = (2 * kPi / W) * (kPi / H) * std::cos(lat);
            cell[static_cast<size_t>(y) * W + x] = s * (om / (k * k));
        }
    auto dirOf = [&](double x, double y) {
        const double lat = kPi * (0.5 - y / H), lon = 2 * kPi * x / W - kPi - yaw;
        return V{std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
    };
    std::vector<size_t> order(cell.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto lum = [](V e) { return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z; };
    std::partial_sort(order.begin(), order.begin() + 1200, order.end(),
                      [&](size_t a, size_t b) { return lum(cell[a]) > lum(cell[b]); });
    std::vector<Light> lights;
    std::vector<char> taken(cell.size(), 0);
    for (int i = 0; i < 1200; ++i) {
        const size_t c = order[static_cast<size_t>(i)];
        taken[c] = 1;
        lights.push_back({dirOf(c % W + 0.5, c / W + 0.5), cell[c]});
    }
    const int cw = W / 64, chh = H / 32;
    for (int gy = 0; gy < 32; ++gy)
        for (int gx = 0; gx < 64; ++gx) {
            V e{0, 0, 0}, d{0, 0, 0};
            for (int y = gy * chh; y < (gy + 1) * chh; ++y)
                for (int x = gx * cw; x < (gx + 1) * cw; ++x) {
                    const size_t c = static_cast<size_t>(y) * W + x;
                    if (taken[c]) continue;
                    e = e + cell[c];
                    d = d + dirOf(x + 0.5, y + 0.5) * (lum(cell[c]) + 1e-12);
                }
            if (lum(e) > 0) lights.push_back({norm(d), e});
        }
    return lights;
}

// Scene: floor y=0, matte sphere, glossy sphere, white cube.
struct Sphere { V c; double r; };
const Sphere kMatte{{-1.35, 0.6, 0.0}, 0.6};
const Sphere kGloss{{0.05, 0.6, -0.2}, 0.6};
const V kBoxMin{0.95, 0.0, -0.45}, kBoxMax{1.85, 0.9, 0.45};

double HitSphere(const Sphere& s, V o, V d) {
    V oc = o - s.c;
    double b = dot(oc, d), c = dot(oc, oc) - s.r * s.r, h = b * b - c;
    if (h < 0) return -1;
    h = std::sqrt(h);
    double t = -b - h;
    if (t > 1e-6) return t;
    t = -b + h;
    return t > 1e-6 ? t : -1;
}
double HitBox(V o, V d, V* n) {
    double t0 = 1e-6, t1 = 1e30;
    int axis = -1; double sign = 0;
    const double oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    const double lo[3] = {kBoxMin.x, kBoxMin.y, kBoxMin.z}, hi[3] = {kBoxMax.x, kBoxMax.y, kBoxMax.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dd[a]) < 1e-12) { if (oo[a] < lo[a] || oo[a] > hi[a]) return -1; continue; }
        double ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
        double s = -1;
        if (ta > tb) { std::swap(ta, tb); s = 1; }
        if (ta > t0) { t0 = ta; axis = a; sign = s; }
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1;
    }
    if (axis < 0) return -1;
    if (n) { *n = {0, 0, 0}; (axis == 0 ? n->x : axis == 1 ? n->y : n->z) = sign; }
    return t0;
}
bool Occluded(V p, V l) {
    if (HitSphere(kMatte, p, l) > 0 || HitSphere(kGloss, p, l) > 0) return true;
    return HitBox(p, l, nullptr) > 0;
}

V Diffuse(V p, V n, const std::vector<Light>& lights, double albedo) {
    V s{0, 0, 0};
    for (const Light& L : lights) {
        const double c = dot(n, L.dir);
        if (c <= 0) continue;
        if (Occluded(p + n * 1e-4, L.dir)) continue;
        s = s + L.e * c;
    }
    return s * (albedo / kPi);
}

V Glossy(V p, V n, V view, const std::vector<Light>& lights) {
    const V r = view - n * (2 * dot(view, n));
    const double ex = 400.0;
    V s{0, 0, 0};
    for (const Light& L : lights) {
        const double c = dot(r, L.dir);
        if (c <= 0 || dot(n, L.dir) <= 0) continue;
        if (Occluded(p + n * 1e-4, L.dir)) continue;
        s = s + L.e * (std::pow(c, ex) * (ex + 2) / (2 * kPi));
    }
    return s * 0.04 + Diffuse(p, n, lights, 0.08);   // a dark, shiny paint
}

struct Shot { std::vector<V> px; double litFloor = 0, shadowFloor = 0; };

Shot Render(const Env& env, const std::vector<Light>& lights, int W, int H) {
    Shot s;
    s.px.assign(static_cast<size_t>(W) * H, V{0, 0, 0});
    const V cam{0.2, 1.6, 6.2}, look{0.2, 0.55, 0.0};
    const V f = norm(look - cam), right = norm(V{f.z, 0, -f.x} * -1.0), up = norm(V{right.y * f.z - right.z * f.y,
                                                                                  right.z * f.x - right.x * f.z,
                                                                                  right.x * f.y - right.y * f.x});
    const double fov = std::tan(32.0 * kPi / 180.0);
    ParallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const double u = (2 * (x + 0.5) / W - 1) * fov * W / H, v = (1 - 2 * (y + 0.5) / H) * fov;
                const V d = norm(f + right * u + up * v);
                double best = 1e30; int what = -1; V n{0, 1, 0};
                double t;
                if ((t = HitSphere(kMatte, cam, d)) > 0 && t < best) { best = t; what = 0; }
                if ((t = HitSphere(kGloss, cam, d)) > 0 && t < best) { best = t; what = 1; }
                V bn; if ((t = HitBox(cam, d, &bn)) > 0 && t < best) { best = t; what = 2; n = bn; }
                if (d.y < 0 && (t = -cam.y / d.y) < best) { best = t; what = 3; n = {0, 1, 0}; }
                V c;
                if (what < 0) c = env.Look(d);
                else {
                    const V p = cam + d * best;
                    if (what == 0) c = Diffuse(p, norm(p - kMatte.c), lights, 0.5);
                    else if (what == 1) c = Glossy(p, norm(p - kGloss.c), d, lights);
                    else if (what == 2) c = Diffuse(p, n, lights, 0.8);
                    else {
                        if (std::fabs(p.x) > 9 || p.z < -9 || p.z > 9) c = env.Look(d);   // floor is 18 m wide
                        else c = Diffuse(p, n, lights, 0.5);
                    }
                }
                s.px[static_cast<size_t>(y) * W + x] = c;
            }
    }, W * 400LL);
    // Floor brightness in the sun and inside the cube's shadow (measured on the floor itself).
    auto floorAt = [&](V p) { return Diffuse(p, {0, 1, 0}, lights, 0.5); };
    auto lum = [](V e) { return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z; };
    s.litFloor = lum(floorAt({-0.2, 0, 2.2}));
    s.shadowFloor = -1;   // filled by the caller (needs the sun direction)
    (void)floorAt;
    return s;
}

void SavePng(const Shot& s, int W, int H, double ev, const std::string& path) {
    std::vector<unsigned char> rgb(static_cast<size_t>(W) * H * 3);
    const double k = std::exp2(ev);
    for (size_t i = 0; i < s.px.size(); ++i) {
        const double v[3] = {s.px[i].x * k, s.px[i].y * k, s.px[i].z * k};
        for (int c = 0; c < 3; ++c) {
            // ACES fit (as in EXRay), then sRGB
            const double a = v[c] * 0.6;
            double t = (a * (2.51 * a + 0.03)) / (a * (2.43 * a + 0.59) + 0.14);
            t = std::clamp(t, 0.0, 1.0);
            t = t <= 0.0031308 ? 12.92 * t : 1.055 * std::pow(t, 1 / 2.4) - 0.055;
            rgb[i * 3 + c] = static_cast<unsigned char>(std::lround(t * 255));
        }
    }
    stbi_write_png(path.c_str(), W, H, 3, rgb.data(), W * 3);
}

// Light arriving within `deg` degrees of a direction (pixel x,y) of an equirect image.
double LightNear(const HdrImage& im, double cx, double cy, double deg) {
    const double W = im.width, H = im.height;
    auto dir = [&](double x, double y, double* d) {
        const double lat = kPi * (0.5 - y / H), lon = 2 * kPi * x / W - kPi;
        d[0] = std::cos(lat) * std::sin(lon); d[1] = std::sin(lat); d[2] = std::cos(lat) * std::cos(lon);
    };
    double c[3]; dir(cx, cy, c);
    const double cosMax = std::cos(deg * kPi / 180), r = deg / 180.0 * H + 3;
    double e = 0;
    for (int y = std::max(0, int(cy - r)); y < std::min(im.height, int(cy + r) + 1); ++y) {
        const double lat = kPi * (0.5 - (y + 0.5) / H);
        const double rx = r / std::max(0.05, std::cos(lat));
        for (int xx = int(cx - rx); xx <= int(cx + rx); ++xx) {
            const int x = ((xx % im.width) + im.width) % im.width;
            double d[3]; dir(x + 0.5, y + 0.5, d);
            if (d[0] * c[0] + d[1] * c[1] + d[2] * c[2] < cosMax) continue;
            const float* p = im.rgb() + (size_t(y) * im.width + x) * 3;
            const double om = (2 * kPi / W) * (kPi / H) * std::cos(lat);
            e += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * om;
        }
    }
    return e;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: light_test truth.hdr out_prefix [clipLevel] [strengthEV]\n"); return 1; }
    const float clipAt = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 0.0f;
    const float strength = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 0.0f;
    HdrImage truth; std::string err;
    if (!LoadHdrFile(argv[1], truth, err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    // The "clipped" copy: what a camera that tops out at clipAt would have stored.
    HdrImage clipped;
    clipped.width = truth.width; clipped.height = truth.height; clipped.format = truth.format;
    clipped.pixels.reset(static_cast<float*>(std::malloc(truth.valueCount() * sizeof(float))));
    std::memcpy(clipped.pixels.get(), truth.rgb(), truth.valueCount() * sizeof(float));
    if (clipAt > 0) {
        float* q = clipped.pixels.get();
        for (size_t i = 0; i < size_t(clipped.width) * clipped.height * 3; ++i) q[i] = std::min(q[i], clipAt);
    }
    HdriReport r = AnalyzeHdri(clipped);
    std::printf("input %dx%d max %.0f | clipped %d  sun %d  level %.0f  px %zu  elev %.1f  size %.2f deg  share %.1f%%  peak %.0f\n",
                clipped.width, clipped.height, r.maxValue, r.clipped, static_cast<int>(r.sun), r.clipLevel, r.clippedPixels,
                r.sunElevation, r.sunSizeDeg, 100 * r.lightShare, r.sunPeak);
    if (r.sun != HdriReport::Sun::Rebuildable) { std::printf("nothing to rebuild\n"); return 0; }

    HdriFixOptions o;
    o.sunStrengthEV = strength;
    o.sunColor = r.glowNeutral ? SunColorMode::Natural : SunColorMode::Glow;
    HdrImage fixed; HdriFixResult res;
    if (!FixHdri(clipped, r, o, fixed, res, err)) { std::printf("fix: %s\n", err.c_str()); return 1; }
    const double eT = LightNear(truth, r.sunX, r.sunY, 2), eC = LightNear(clipped, r.sunX, r.sunY, 2),
                 eF = LightNear(fixed, r.sunX, r.sunY, 2);
    std::printf("sun light within 2 deg: truth %.2f  clipped %.2f (%.0f%%)  fixed %.2f (%.0f%%)  fixed peak %.0f\n",
                eT, eC, 100 * eC / eT, eF, 100 * eF / eT, res.sunPeak);

    const double sunLon = (r.sunX / truth.width - 0.5) * 2 * kPi;
    const double yaw = sunLon - (-2.45);
    const int W = 800, H = 450;
    struct Case { const HdrImage* im; const char* name; } cases[] = {{&truth, "1_truth"}, {&clipped, "2_clipped"}, {&fixed, "3_fixed"}};
    double lit[3], shade[3];
    std::vector<Shot> shots;
    auto lum = [](V e) { return 0.2126 * e.x + 0.7152 * e.y + 0.0722 * e.z; };
    // Sun direction in the scene, from the image position (same for all three).
    V sd;
    {
        const double lat = kPi * (0.5 - r.sunY / truth.height), lon = sunLon - yaw;
        sd = V{std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
    }
    // A floor point whose line to the sun passes through the middle of the cube: surely in shadow.
    // A floor point in the cube's shadow, just outside the cube's footprint.
    V shadowPt{0, 0, 0};
    {
        const double hl = std::sqrt(sd.x * sd.x + sd.z * sd.z);
        const V away{-sd.x / hl, 0, -sd.z / hl};
        for (double dist = 0.3; dist < 4; dist += 0.05) {
            const V p = V{1.4, 0, 0} + away * dist;
            const bool inside = p.x > kBoxMin.x - 0.05 && p.x < kBoxMax.x + 0.05 && p.z > kBoxMin.z - 0.05 && p.z < kBoxMax.z + 0.05;
            if (inside) continue;
            if (Occluded(p + V{0, 1e-4, 0}, sd)) { shadowPt = p; break; }
        }
    }
    // A floor point with a clear line to the sun.
    V litPt{-0.2, 0, 2.6};
    for (double z = 2.6; z < 5; z += 0.2) { litPt = {-0.2, 0, z}; if (!Occluded(litPt + V{0, 1e-4, 0}, sd)) break; }
    std::printf("shadow point (%.2f, %.2f)  in shadow %d | lit point (%.2f, %.2f)  in shadow %d\n", shadowPt.x, shadowPt.z,
                Occluded(shadowPt + V{0, 1e-4, 0}, sd), litPt.x, litPt.z, Occluded(litPt + V{0, 1e-4, 0}, sd));
    for (int i = 0; i < 3; ++i) {
        Env env{cases[i].im, yaw};
        std::vector<Light> lights = MakeLights(*cases[i].im, yaw);
        shots.push_back(Render(env, lights, W, H));
        lit[i] = lum(Diffuse(litPt, {0, 1, 0}, lights, 0.5));
        shade[i] = lum(Diffuse(shadowPt, {0, 1, 0}, lights, 0.5));
        std::printf("%-10s floor in sun %.3f  in shadow %.3f  contrast %.2f : 1 (%.2f stops)\n", cases[i].name, lit[i], shade[i],
                    lit[i] / shade[i], std::log2(lit[i] / shade[i]));
    }
    const double ev = std::log2(0.18 / lit[0]);
    for (int i = 0; i < 3; ++i) SavePng(shots[i], W, H, ev, std::string(argv[2]) + "_" + cases[i].name + ".png");
    std::printf("exposure for all %.2f EV\n", ev);
    return 0;
}
