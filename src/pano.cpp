// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pano.h"

#include <algorithm>
#include <cmath>

#include "parallel.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// Camera axes for a view: right, up, forward (unit vectors, y up).
struct Basis { double r[3], u[3], f[3]; };

Basis MakeBasis(const PanoView& v) {
    const double cy = std::cos(v.yaw * kDeg), sy = std::sin(v.yaw * kDeg);
    const double cp = std::cos(v.pitch * kDeg), sp = std::sin(v.pitch * kDeg);
    Basis b;
    // Forward at yaw 0, pitch 0 is +z (the centre of the image).
    b.f[0] = sy * cp; b.f[1] = sp;  b.f[2] = cy * cp;
    b.r[0] = cy;      b.r[1] = 0.0; b.r[2] = -sy;
    // up = forward x right... chosen so that +pitch tilts the view upward.
    b.u[0] = -sy * sp; b.u[1] = cp; b.u[2] = -cy * sp;
    return b;
}

// Direction through screen point (sx, sy) -> (u, v) in 0..1 of the panorama.
inline void DirToUV(const Basis& b, double tx, double ty, double& u, double& v) {
    const double dx = b.f[0] + b.r[0] * tx + b.u[0] * ty;
    const double dy = b.f[1] + b.r[1] * tx + b.u[1] * ty;
    const double dz = b.f[2] + b.r[2] * tx + b.u[2] * ty;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double lon = std::atan2(dx, dz);                       // -pi..pi, 0 = image centre
    const double lat = std::asin(std::clamp(dy / len, -1.0, 1.0)); // -pi/2..pi/2
    u = 0.5 + lon / (2.0 * kPi);
    v = 0.5 - lat / kPi;
}

}  // namespace

void NormalizePanoView(PanoView& v) {
    v.yaw = std::fmod(v.yaw, 360.0);
    if (v.yaw > 180.0) v.yaw -= 360.0;
    if (v.yaw < -180.0) v.yaw += 360.0;
    v.pitch = std::clamp(v.pitch, -89.9, 89.9);
    v.fov = std::clamp(v.fov, kPanoMinFov, kPanoMaxFov);
}

void RenderPano(const PanoSource* levels, int levelCount, const PanoView& view,
                uint8_t* dst, int dstW, int dstH) {
    if (!levels || levelCount <= 0 || dstW <= 0 || dstH <= 0) return;
    const double tanH = std::tan(view.fov * 0.5 * kDeg);
    const double tanV = tanH * dstH / dstW;

    // Screen pixels per radian at the centre; pick the smallest level with at
    // least that many source pixels per radian (so it's sharp but not noisy).
    const double needed = (dstW * 0.5) / tanH * 2.0 * kPi;
    int li = 0;
    for (int i = 0; i < levelCount; ++i)
        if (levels[i].width >= needed * 0.9) li = i;
    const PanoSource& src = levels[li];
    const int W = src.width, H = src.height;
    if (!src.bgra || W <= 1 || H <= 1) return;
    const Basis b = MakeBasis(view);

    ParallelRows(dstH, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double ty = (1.0 - 2.0 * (y + 0.5) / dstH) * tanV;
            uint8_t* out = dst + static_cast<size_t>(y) * dstW * 4;
            for (int x = 0; x < dstW; ++x) {
                const double tx = (2.0 * (x + 0.5) / dstW - 1.0) * tanH;
                double u, v;
                DirToUV(b, tx, ty, u, v);
                // Bilinear sample; wraps around horizontally, clamps at the poles.
                const double fx = u * W - 0.5, fy = std::clamp(v * H - 0.5, 0.0, H - 1.0);
                int x0 = static_cast<int>(std::floor(fx));
                const int yy0 = static_cast<int>(fy), yy1 = std::min(H - 1, yy0 + 1);
                const double ax = fx - x0, ay = fy - yy0;
                x0 = ((x0 % W) + W) % W;
                const int x1 = (x0 + 1) % W;
                const uint8_t* p00 = src.bgra + (static_cast<size_t>(yy0) * W + x0) * 4;
                const uint8_t* p01 = src.bgra + (static_cast<size_t>(yy0) * W + x1) * 4;
                const uint8_t* p10 = src.bgra + (static_cast<size_t>(yy1) * W + x0) * 4;
                const uint8_t* p11 = src.bgra + (static_cast<size_t>(yy1) * W + x1) * 4;
                const double w00 = (1 - ax) * (1 - ay), w01 = ax * (1 - ay), w10 = (1 - ax) * ay, w11 = ax * ay;
                for (int c = 0; c < 3; ++c)
                    out[x * 4 + c] = static_cast<uint8_t>(p00[c] * w00 + p01[c] * w01 + p10[c] * w10 + p11[c] * w11 + 0.5);
                out[x * 4 + 3] = 255;
            }
        }
    }, dstW * 8);
}

void PanoScreenToImage(const PanoView& view, int viewW, int viewH, double sx, double sy,
                       int imgW, int imgH, int& x, int& y) {
    const double tanH = std::tan(view.fov * 0.5 * kDeg);
    const double tanV = tanH * viewH / std::max(1, viewW);
    const double tx = (2.0 * sx / std::max(1, viewW) - 1.0) * tanH;
    const double ty = (1.0 - 2.0 * sy / std::max(1, viewH)) * tanV;
    double u, v;
    DirToUV(MakeBasis(view), tx, ty, u, v);
    x = std::clamp(static_cast<int>(std::floor(u * imgW)), 0, imgW - 1);
    y = std::clamp(static_cast<int>(std::floor(v * imgH)), 0, imgH - 1);
}
