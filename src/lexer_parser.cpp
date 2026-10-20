// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "../include/mesh.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mesh {

// ============================================================================
// 1. Perceptual Color Science (sRGB <-> Linear sRGB <-> OKLab <-> OKLCH)
// ============================================================================

static inline double srgb_to_linear(double c) {
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

static const auto& get_linear_to_srgb_lut() {
    static const auto lut = []() {
        std::vector<float> table(4097);
        for (int i = 0; i <= 4096; ++i) {
            double c = static_cast<double>(i) / 4096.0;
            double s = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
            table[i] = static_cast<float>(s);
        }
        return table;
    }();
    return lut;
}

static inline double linear_to_srgb(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    const auto& lut = get_linear_to_srgb_lut();
    double f = c * 4096.0;
    int idx = static_cast<int>(f);
    double frac = f - idx;
    return lut[idx] + (lut[idx + 1] - lut[idx]) * frac;
}

static void linear_srgb_to_oklab(double r, double g, double b, double& L, double& a, double& b_out) {
    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

    double l_ = std::cbrt(std::max(0.0, l));
    double m_ = std::cbrt(std::max(0.0, m));
    double s_ = std::cbrt(std::max(0.0, s));

    L     = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    a     = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    b_out = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
}

static void oklab_to_linear_srgb(double L, double a, double b_in, double& r, double& g, double& b) {
    double l_ = L + 0.3963377774 * a + 0.2158037573 * b_in;
    double m_ = L - 0.1055613458 * a - 0.0638541728 * b_in;
    double s_ = L - 0.0894841775 * a - 1.2914855480 * b_in;

    double l = l_ * l_ * l_;
    double m = m_ * m_ * m_;
    double s = s_ * s_ * s_;

    r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

Color Color::from_srgb(double r, double g, double b, double a) {
    Color c;
    c.r = clamp01(r);
    c.g = clamp01(g);
    c.b = clamp01(b);
    c.a = clamp01(a);
    double lr = srgb_to_linear(c.r);
    double lg = srgb_to_linear(c.g);
    double lb = srgb_to_linear(c.b);
    linear_srgb_to_oklab(lr, lg, lb, c.ok_L, c.ok_a, c.ok_b);
    return c;
}

static inline double soft_gamut_channel(double x) {
    const double eps = 0.003;
    double pos = 0.5 * (x + std::sqrt(x * x + eps * eps));
    double d1 = 1.0 - pos;
    return 1.0 - 0.5 * (d1 + std::sqrt(d1 * d1 + eps * eps));
}

Color Color::from_oklab(double L, double a_lab, double b_lab, double alpha) {
    Color c;
    c.ok_L = clamp01(L);
    c.ok_a = a_lab;
    c.ok_b = b_lab;
    c.a = clamp01(alpha);
    double lr, lg, lb;
    oklab_to_linear_srgb(c.ok_L, c.ok_a, c.ok_b, lr, lg, lb);
    if ((lr < 0.0 || lr > 1.0 || lg < 0.0 || lg > 1.0 || lb < 0.0 || lb > 1.0) &&
        (c.ok_a * c.ok_a + c.ok_b * c.ok_b > 1e-6)) {
        lr = soft_gamut_channel(lr);
        lg = soft_gamut_channel(lg);
        lb = soft_gamut_channel(lb);
    }
    c.r = linear_to_srgb(lr);
    c.g = linear_to_srgb(lg);
    c.b = linear_to_srgb(lb);
    return c;
}

Color Color::from_oklch(double L, double C, double h_deg, double a) {
    L = clamp01(L);
    C = std::max(0.0, C);
    double h_rad = h_deg * PI / 180.0;
    double cs = std::cos(h_rad);
    double sn = std::sin(h_rad);

    auto in_gamut = [&](double test_c) {
        double lr, lg, lb;
        oklab_to_linear_srgb(L, test_c * cs, test_c * sn, lr, lg, lb);
        return (lr >= 0.002 && lr <= 0.998 &&
                lg >= 0.002 && lg <= 0.998 &&
                lb >= 0.002 && lb <= 0.998);
    };

    double mapped_c = C;
    if (!in_gamut(mapped_c)) {
        double lo = 0.0, hi = C;
        for (int iter = 0; iter < 14; ++iter) {
            double mid = 0.5 * (lo + hi);
            if (in_gamut(mid)) lo = mid;
            else hi = mid;
        }
        mapped_c = lo * 0.96;
    }

    return from_oklab(L, mapped_c * cs, mapped_c * sn, a);
}

Color Color::from_hsl(double h_deg, double s, double l, double a) {
    h_deg = std::fmod(h_deg, 360.0);
    if (h_deg < 0) h_deg += 360.0;
    s = clamp01(s);
    l = clamp01(l);
    double c = (1.0 - std::abs(2.0 * l - 1.0)) * s;
    double x = c * (1.0 - std::abs(std::fmod(h_deg / 60.0, 2.0) - 1.0));
    double m = l - c * 0.5;
    double r1 = 0, g1 = 0, b1 = 0;
    if (h_deg < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h_deg < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h_deg < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h_deg < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h_deg < 300) { r1 = x; g1 = 0; b1 = c; }
    else                  { r1 = c; g1 = 0; b1 = x; }
    return from_srgb(r1 + m, g1 + m, b1 + m, a);
}

static int hex_val(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return 0;
}

Color Color::from_hex(const std::string& hex_str) {
    std::string s = hex_str;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() == 3) {
        double r = (hex_val(s[0]) * 17) / 255.0;
        double g = (hex_val(s[1]) * 17) / 255.0;
        double b = (hex_val(s[2]) * 17) / 255.0;
        return from_srgb(r, g, b, 1.0);
    } else if (s.size() == 4) {
        double r = (hex_val(s[0]) * 17) / 255.0;
        double g = (hex_val(s[1]) * 17) / 255.0;
        double b = (hex_val(s[2]) * 17) / 255.0;
        double a = (hex_val(s[3]) * 17) / 255.0;
        return from_srgb(r, g, b, a);
    } else if (s.size() == 6) {
        double r = (hex_val(s[0]) * 16 + hex_val(s[1])) / 255.0;
        double g = (hex_val(s[2]) * 16 + hex_val(s[3])) / 255.0;
        double b = (hex_val(s[4]) * 16 + hex_val(s[5])) / 255.0;
        return from_srgb(r, g, b, 1.0);
    } else if (s.size() == 8) {
        double r = (hex_val(s[0]) * 16 + hex_val(s[1])) / 255.0;
        double g = (hex_val(s[2]) * 16 + hex_val(s[3])) / 255.0;
        double b = (hex_val(s[4]) * 16 + hex_val(s[5])) / 255.0;
        double a = (hex_val(s[6]) * 16 + hex_val(s[7])) / 255.0;
        return from_srgb(r, g, b, a);
    }
    return from_srgb(0.0, 0.0, 0.0, 1.0);
}

Color Color::lerp_oklab(const Color& c0, const Color& c1, double t) {
    t = clamp01(t);
    double L = c0.ok_L + (c1.ok_L - c0.ok_L) * t;
    double a = c0.ok_a + (c1.ok_a - c0.ok_a) * t;
    double b = c0.ok_b + (c1.ok_b - c0.ok_b) * t;
    double alpha = c0.a + (c1.a - c0.a) * t;
    return from_oklab(L, a, b, alpha);
}

Color Color::lighten(double delta_L) const {
    return from_oklab(clamp01(ok_L + delta_L), ok_a, ok_b, a);
}

Color Color::darken(double delta_L) const {
    return from_oklab(clamp01(ok_L - delta_L), ok_a, ok_b, a);
}

Color Color::with_alpha(double new_alpha) const {
    return from_oklab(ok_L, ok_a, ok_b, clamp01(new_alpha));
}

std::string Color::to_css_rgba() const {
    int ri = static_cast<int>(std::round(clamp01(r) * 255.0));
    int gi = static_cast<int>(std::round(clamp01(g) * 255.0));
    int bi = static_cast<int>(std::round(clamp01(b) * 255.0));
    std::ostringstream oss;
    oss << "rgba(" << ri << "," << gi << "," << bi << "," << std::fixed << std::setprecision(3) << clamp01(a) << ")";
    return oss.str();
}

std::string Color::to_hex() const {
    int ri = static_cast<int>(std::round(clamp01(r) * 255.0));
    int gi = static_cast<int>(std::round(clamp01(g) * 255.0));
    int bi = static_cast<int>(std::round(clamp01(b) * 255.0));
    std::ostringstream oss;
    oss << "#" << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << ri << std::setw(2) << gi << std::setw(2) << bi;
    return oss.str();
}

// ============================================================================
// 2. Procedural FBM Noise & PaintServer Evaluation
// ============================================================================

static inline double hash2d(int x, int y, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(seed) * 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (h & 0xFFFFFFu) / static_cast<double>(0x1000000u);
}

static double value_noise_2d(double x, double y, int seed) {
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    double fx = x - ix;
    double fy = y - iy;
    double u = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0);
    double v = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0);

    double n00 = hash2d(ix,     iy,     seed);
    double n10 = hash2d(ix + 1, iy,     seed);
    double n01 = hash2d(ix,     iy + 1, seed);
    double n11 = hash2d(ix + 1, iy + 1, seed);

    double nx0 = n00 + (n10 - n00) * u;
    double nx1 = n01 + (n11 - n01) * u;
    return nx0 + (nx1 - nx0) * v;
}

double fbm_noise_2d(double x, double y, int octaves, double persistence, double lacunarity, double seed) {
    int oct = std::clamp(octaves, 1, 8);
    int iseed = static_cast<int>(std::round(seed * 100.0));
    double total = 0.0;
    double amplitude = 1.0;
    double max_amp = 0.0;
    double freq = 1.0;

    for (int i = 0; i < oct; ++i) {
        total += value_noise_2d(x * freq, y * freq, iseed + i * 101) * amplitude;
        max_amp += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return max_amp > 1e-9 ? clamp01(total / max_amp) : 0.0;
}

static Color sample_stops_oklab(const std::vector<GradientStop>& stops, double t) {
    if (stops.empty()) return Color::from_srgb(0, 0, 0, 1);
    if (stops.size() == 1 || t <= stops.front().offset) return stops.front().color;
    if (t >= stops.back().offset) return stops.back().color;
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        if (t >= stops[i].offset && t <= stops[i + 1].offset) {
            double span = stops[i + 1].offset - stops[i].offset;
            double local_t = span > 1e-9 ? (t - stops[i].offset) / span : 0.0;
            return Color::lerp_oklab(stops[i].color, stops[i + 1].color, local_t);
        }
    }
    return stops.back().color;
}

static inline Color sample_stop_lut_fast(const std::vector<Color>& lut, const std::vector<GradientStop>& stops, double t) {
    if (lut.size() == 256) {
        double f = clamp01(t) * 255.0;
        int idx = static_cast<int>(f);
        if (idx >= 255) return lut[255];
        double frac = f - idx;
        const Color& c0 = lut[idx];
        const Color& c1 = lut[idx + 1];
        Color out;
        out.r = c0.r + (c1.r - c0.r) * frac;
        out.g = c0.g + (c1.g - c0.g) * frac;
        out.b = c0.b + (c1.b - c0.b) * frac;
        out.a = c0.a + (c1.a - c0.a) * frac;
        return out;
    }
    return sample_stops_oklab(stops, clamp01(t));
}

void PaintServer::build_luts() {
    if (kind == PaintKind::LinearGradient || kind == PaintKind::RadialGradient ||
        kind == PaintKind::ConicGradient || kind == PaintKind::NoiseGradient) {
        stop_lut.resize(256);
        for (int i = 0; i < 256; ++i) {
            stop_lut[i] = sample_stops_oklab(stops, static_cast<double>(i) / 255.0);
        }
    }
    if (kind == PaintKind::NoiseGradient) {
        diff_grid_lut.resize(64 * 64);
        for (int gy = 0; gy < 64; ++gy) {
            double v = static_cast<double>(gy) / 63.0;
            for (int gx = 0; gx < 64; ++gx) {
                double u = static_cast<double>(gx) / 63.0;
                double n = fbm_noise_2d(u * noise_scale, v * noise_scale,
                                        noise_octaves, noise_persistence, noise_lacunarity, noise_seed);
                diff_grid_lut[gy * 64 + gx] = sample_stop_lut_fast(stop_lut, stops, n);
            }
        }
    } else if (kind == PaintKind::DiffusionGradient) {
        diff_grid_lut.resize(64 * 64);
        const bool fast_p13 = (std::abs(0.5 * std::max(1.0, falloff_exponent) - 1.3) < 0.25);
        double half_p = 0.5 * std::max(1.0, falloff_exponent);
        for (int gy = 0; gy < 64; ++gy) {
            double v = static_cast<double>(gy) / 63.0;
            for (int gx = 0; gx < 64; ++gx) {
                double u = static_cast<double>(gx) / 63.0;
                if (diffusion_points.empty()) {
                    diff_grid_lut[gy * 64 + gx] = Color::from_srgb(0, 0, 0, 1);
                    continue;
                }
                double sum_w = 0.0, sum_L = 0.0, sum_a = 0.0, sum_b = 0.0, sum_alpha = 0.0;
                for (const auto& pt : diffusion_points) {
                    double dx = u - pt.u;
                    double dy = v - pt.v;
                    double d2 = dx * dx + dy * dy + 0.0025;
                    double w = fast_p13 ? (pt.weight / (d2 * std::sqrt(std::sqrt(d2))))
                                        : (pt.weight / std::pow(d2, half_p));
                    sum_w += w;
                    sum_L += w * pt.color.ok_L;
                    sum_a += w * pt.color.ok_a;
                    sum_b += w * pt.color.ok_b;
                    sum_alpha += w * pt.color.a;
                }
                diff_grid_lut[gy * 64 + gx] = sum_w > 1e-12
                    ? Color::from_oklab(sum_L / sum_w, sum_a / sum_w, sum_b / sum_w, sum_alpha / sum_w)
                    : diffusion_points.front().color;
            }
        }
    }
}


Color PaintServer::sample(const Vec2& p_world, const BBox& bbox) const {
    if (kind == PaintKind::None) {
        return Color::from_srgb(0, 0, 0, 0);
    }
    if (kind == PaintKind::Solid) {
        return solid_color;
    }

    double bw = std::max(1.0, bbox.width());
    double bh = std::max(1.0, bbox.height());
    Vec2 uv = user_space ? p_world : Vec2{(p_world.x - bbox.min_x) / bw, (p_world.y - bbox.min_y) / bh};

    if (kind == PaintKind::LinearGradient) {
        Vec2 d = end - start;
        double len_sq = d.length_sq();
        double t = len_sq > 1e-12 ? (uv - start).dot(d) / len_sq : 0.0;
        return sample_stop_lut_fast(stop_lut, stops, t);
    }

    if (kind == PaintKind::RadialGradient) {
        double r = std::max(1e-6, radius);
        double t = (uv - center).length() / r;
        return sample_stop_lut_fast(stop_lut, stops, t);
    }

    if (kind == PaintKind::ConicGradient) {
        Vec2 d = uv - center;
        double ang = std::atan2(d.y, d.x) * 180.0 / PI - angle_deg;
        ang = std::fmod(ang, 360.0);
        if (ang < 0) ang += 360.0;
        double t = ang / 360.0;
        return sample_stop_lut_fast(stop_lut, stops, t);
    }

    if (kind == PaintKind::NoiseGradient && diff_grid_lut.size() == 64 * 64) {
        double gx = clamp01(uv.x) * 63.0;
        double gy = clamp01(uv.y) * 63.0;
        int x0 = std::min(static_cast<int>(gx), 62);
        int y0 = std::min(static_cast<int>(gy), 62);
        double fx = gx - x0;
        double fy = gy - y0;
        const Color& c00 = diff_grid_lut[y0 * 64 + x0];
        const Color& c10 = diff_grid_lut[y0 * 64 + x0 + 1];
        const Color& c01 = diff_grid_lut[(y0 + 1) * 64 + x0];
        const Color& c11 = diff_grid_lut[(y0 + 1) * 64 + x0 + 1];
        double w00 = (1.0 - fx) * (1.0 - fy);
        double w10 = fx * (1.0 - fy);
        double w01 = (1.0 - fx) * fy;
        double w11 = fx * fy;
        Color out;
        out.r = c00.r * w00 + c10.r * w10 + c01.r * w01 + c11.r * w11;
        out.g = c00.g * w00 + c10.g * w10 + c01.g * w01 + c11.g * w11;
        out.b = c00.b * w00 + c10.b * w10 + c01.b * w01 + c11.b * w11;
        out.a = c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11;
        return out;
    }

    if (kind == PaintKind::DiffusionGradient && diff_grid_lut.size() == 64 * 64) {
        double gx = clamp01(uv.x) * 63.0;
        double gy = clamp01(uv.y) * 63.0;
        int x0 = std::min(static_cast<int>(gx), 62);
        int y0 = std::min(static_cast<int>(gy), 62);
        double fx = gx - x0;
        double fy = gy - y0;
        const Color& c00 = diff_grid_lut[y0 * 64 + x0];
        const Color& c10 = diff_grid_lut[y0 * 64 + x0 + 1];
        const Color& c01 = diff_grid_lut[(y0 + 1) * 64 + x0];
        const Color& c11 = diff_grid_lut[(y0 + 1) * 64 + x0 + 1];
        double w00 = (1.0 - fx) * (1.0 - fy);
        double w10 = fx * (1.0 - fy);
        double w01 = (1.0 - fx) * fy;
        double w11 = fx * fy;
        Color out;
        out.r = c00.r * w00 + c10.r * w10 + c01.r * w01 + c11.r * w11;
        out.g = c00.g * w00 + c10.g * w10 + c01.g * w01 + c11.g * w11;
        out.b = c00.b * w00 + c10.b * w10 + c01.b * w01 + c11.b * w11;
        out.a = c00.a * w00 + c10.a * w10 + c01.a * w01 + c11.a * w11;
        return out;
    }

    if (kind == PaintKind::MeshGradient) {
        if (grid_points.empty()) return Color::from_srgb(0, 0, 0, 1);
        int r_cnt = std::max(2, rows);
        int c_cnt = std::max(2, cols);
        if (static_cast<int>(grid_points.size()) >= r_cnt * c_cnt) {
            double u = clamp01(uv.x) * (c_cnt - 1);
            double v = clamp01(uv.y) * (r_cnt - 1);
            int c0 = std::min(static_cast<int>(std::floor(u)), c_cnt - 2);
            int r0 = std::min(static_cast<int>(std::floor(v)), r_cnt - 2);
            double fu = u - c0;
            double fv = v - r0;
            double su = fu * fu * (3.0 - 2.0 * fu);
            double sv = fv * fv * (3.0 - 2.0 * fv);

            const Color& p00 = grid_points[r0 * c_cnt + c0].color;
            const Color& p10 = grid_points[r0 * c_cnt + (c0 + 1)].color;
            const Color& p01 = grid_points[(r0 + 1) * c_cnt + c0].color;
            const Color& p11 = grid_points[(r0 + 1) * c_cnt + (c0 + 1)].color;

            Color top = Color::lerp_oklab(p00, p10, su);
            Color bot = Color::lerp_oklab(p01, p11, su);
            return Color::lerp_oklab(top, bot, sv);
        }
    }

    const auto& pts = (kind == PaintKind::MeshGradient) ? grid_points : diffusion_points;
    if (pts.empty()) return Color::from_srgb(0, 0, 0, 1);

    double sum_w = 0.0;
    double sum_L = 0.0, sum_a = 0.0, sum_b = 0.0, sum_alpha = 0.0;
    double half_p = 0.5 * std::max(1.0, falloff_exponent);
    const bool fast_p13 = (std::abs(half_p - 1.3) < 0.25);

    for (const auto& pt : pts) {
        double dx = uv.x - pt.u;
        double dy = uv.y - pt.v;
        double d2 = dx * dx + dy * dy + 0.0025;
        double w = fast_p13 ? (pt.weight / (d2 * std::sqrt(std::sqrt(d2))))
                            : (pt.weight / std::pow(d2, half_p));
        sum_w += w;
        sum_L += w * pt.color.ok_L;
        sum_a += w * pt.color.ok_a;
        sum_b += w * pt.color.ok_b;
        sum_alpha += w * pt.color.a;
    }

    if (sum_w <= 1e-12) return pts.front().color;
    return Color::from_oklab(sum_L / sum_w, sum_a / sum_w, sum_b / sum_w, sum_alpha / sum_w);
}

// ============================================================================
// 3. Centripetal Catmull-Rom Splines, Arc-Length Tables & Geometry Builders
// ============================================================================

std::vector<PathSegment> spline_to_cubics(const std::vector<Vec2>& pts, double tension, bool closed) {
    std::vector<PathSegment> segs;
    if (pts.size() < 2) return segs;

    PathSegment move_seg;
    move_seg.verb = PathVerb::MoveTo;
    move_seg.p1 = pts[0];
    segs.push_back(move_seg);

    if (pts.size() == 2) {
        PathSegment line_seg;
        line_seg.verb = PathVerb::LineTo;
        line_seg.p1 = pts[1];
        segs.push_back(line_seg);
        return segs;
    }

    double tau = std::clamp(tension, 0.0, 1.5);
    size_t n = pts.size();
    size_t count = closed ? n : (n - 1);

    for (size_t i = 0; i < count; ++i) {
        Vec2 p0 = closed ? pts[(i + n - 1) % n] : (i == 0 ? pts[0] : pts[i - 1]);
        Vec2 p1 = pts[i];
        Vec2 p2 = pts[(i + 1) % n];
        Vec2 p3 = closed ? pts[(i + 2) % n] : ((i + 2 < n) ? pts[i + 2] : pts[n - 1]);

        Vec2 c1 = p1 + (p2 - p0) * (tau / 3.0);
        Vec2 c2 = p2 - (p3 - p1) * (tau / 3.0);

        PathSegment cub;
        cub.verb = PathVerb::CubicTo;
        cub.p1 = c1;
        cub.p2 = c2;
        cub.p3 = p2;
        segs.push_back(cub);
    }

    if (closed) {
        PathSegment cl;
        cl.verb = PathVerb::Close;
        segs.push_back(cl);
    }
    return segs;
}

static void flatten_cubic_recursive(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3,
                                    double tol_sq, int depth, std::vector<Vec2>& out) {
    if (depth < 10) {
        Vec2 u = p1 * 3.0 - p0 * 2.0 - p3;
        Vec2 v = p2 * 3.0 - p3 * 2.0 - p0;
        double err = std::max(u.x * u.x, v.x * v.x) + std::max(u.y * u.y, v.y * v.y);
        if (err > tol_sq) {
            Vec2 p01 = (p0 + p1) * 0.5;
            Vec2 p12 = (p1 + p2) * 0.5;
            Vec2 p23 = (p2 + p3) * 0.5;
            Vec2 p012 = (p01 + p12) * 0.5;
            Vec2 p123 = (p12 + p23) * 0.5;
            Vec2 p0123 = (p012 + p123) * 0.5;
            flatten_cubic_recursive(p0, p01, p012, p0123, tol_sq, depth + 1, out);
            flatten_cubic_recursive(p0123, p123, p23, p3, tol_sq, depth + 1, out);
            return;
        }
    }
    out.push_back(p3);
}

static void append_bulge_arc(const Vec2& start, const Vec2& end, double bulge, std::vector<Vec2>& out) {
    if (std::abs(bulge) < 1e-6 || (end - start).length_sq() < 1e-9) {
        out.push_back(end);
        return;
    }
    double theta = 4.0 * std::atan(bulge);
    Vec2 chord = end - start;
    double d = chord.length();
    double r = d / (2.0 * std::sin(std::abs(theta) * 0.5));
    Vec2 mid = (start + end) * 0.5;
    Vec2 normal = Vec2{-chord.y / d, chord.x / d};
    double cot_half = 1.0 / std::tan(theta * 0.5);
    Vec2 center = mid + normal * (d * 0.5 * cot_half);

    double a0 = std::atan2(start.y - center.y, start.x - center.x);
    int steps = std::max(8, static_cast<int>(std::ceil(std::abs(theta) * r / 1.5)));
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        double a = a0 + theta * t;
        out.push_back({center.x + std::abs(r) * std::cos(a), center.y + std::abs(r) * std::sin(a)});
    }
}

static void append_svg_arc(const Vec2& start, const Vec2& end, double radius_in, bool cw, bool large_arc,
                           std::vector<Vec2>& out) {
    Vec2 chord = end - start;
    double d = chord.length();
    if (d < 1e-6 || radius_in <= 1e-6) {
        out.push_back(end);
        return;
    }
    double r = std::max(radius_in, d * 0.5);
    double h = std::sqrt(std::max(0.0, r * r - (d * d * 0.25)));
    Vec2 mid = (start + end) * 0.5;
    Vec2 perp = Vec2{-chord.y / d, chord.x / d};
    double sign = (cw != large_arc) ? 1.0 : -1.0;
    Vec2 center = mid + perp * (sign * h);

    double a0 = std::atan2(start.y - center.y, start.x - center.x);
    double a1 = std::atan2(end.y - center.y, end.x - center.x);
    double delta = a1 - a0;
    if (cw && delta < 0) delta += 2.0 * PI;
    if (!cw && delta > 0) delta -= 2.0 * PI;

    int steps = std::max(8, static_cast<int>(std::ceil(std::abs(delta) * r / 1.5)));
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        double a = a0 + delta * t;
        out.push_back({center.x + r * std::cos(a), center.y + r * std::sin(a)});
    }
}

static void populate_arc_lengths(FlattenedSubpath& sp) {
    sp.cumulative_lengths.resize(sp.points.size(), 0.0);
    double acc = 0.0;
    for (size_t i = 1; i < sp.points.size(); ++i) {
        acc += (sp.points[i] - sp.points[i - 1]).length();
        sp.cumulative_lengths[i] = acc;
    }
}

static std::vector<FlattenedSubpath> flatten_path_segments(const std::vector<PathSegment>& segs) {
    std::vector<FlattenedSubpath> result;
    FlattenedSubpath current;
    Vec2 cur_pt = {0.0, 0.0};
    Vec2 start_pt = {0.0, 0.0};

    for (const auto& s : segs) {
        switch (s.verb) {
            case PathVerb::MoveTo: {
                if (!current.points.empty()) {
                    populate_arc_lengths(current);
                    result.push_back(current);
                    current = FlattenedSubpath{};
                }
                cur_pt = start_pt = s.p1;
                current.points.push_back(cur_pt);
                break;
            }
            case PathVerb::LineTo: {
                if (current.points.empty()) current.points.push_back(cur_pt);
                cur_pt = s.p1;
                current.points.push_back(cur_pt);
                break;
            }
            case PathVerb::QuadTo: {
                if (current.points.empty()) current.points.push_back(cur_pt);
                Vec2 c1 = cur_pt + (s.p1 - cur_pt) * (2.0 / 3.0);
                Vec2 c2 = s.p2 + (s.p1 - s.p2) * (2.0 / 3.0);
                flatten_cubic_recursive(cur_pt, c1, c2, s.p2, 2.25, 0, current.points);
                cur_pt = s.p2;
                break;
            }
            case PathVerb::CubicTo: {
                if (current.points.empty()) current.points.push_back(cur_pt);
                flatten_cubic_recursive(cur_pt, s.p1, s.p2, s.p3, 2.25, 0, current.points);
                cur_pt = s.p3;
                break;
            }
            case PathVerb::BulgeTo: {
                if (current.points.empty()) current.points.push_back(cur_pt);
                append_bulge_arc(cur_pt, s.p1, s.bulge, current.points);
                cur_pt = s.p1;
                break;
            }
            case PathVerb::ArcTo: {
                if (current.points.empty()) current.points.push_back(cur_pt);
                append_svg_arc(cur_pt, s.p1, s.radius, s.clockwise, s.large_arc, current.points);
                cur_pt = s.p1;
                break;
            }
            case PathVerb::Close: {
                if (!current.points.empty()) {
                    current.closed = true;
                    if ((current.points.back() - start_pt).length_sq() > 1e-6) {
                        current.points.push_back(start_pt);
                    }
                    cur_pt = start_pt;
                    populate_arc_lengths(current);
                    result.push_back(current);
                    current = FlattenedSubpath{};
                }
                break;
            }
        }
    }
    if (!current.points.empty()) {
        populate_arc_lengths(current);
        result.push_back(current);
    }
    return result;
}

static void compute_shape_bbox_and_register(ShapeNode& node, Scene* scene_reg = nullptr) {
    node.inv_transform = node.transform.inverse();
    double det = std::abs(node.transform.a * node.transform.d - node.transform.b * node.transform.c);
    node.scale_factor = det > 1e-9 ? std::sqrt(det) : 1.0;

    node.style.dash_period = 0.0;
    for (double dv : node.style.dash_array) {
        if (dv > 0.0) node.style.dash_period += dv;
    }

    BBox b;
    switch (node.kind) {
        case ShapeKind::Rect: {
            Vec2 c[4] = {
                {node.x, node.y},
                {node.x + node.width, node.y},
                {node.x + node.width, node.y + node.height},
                {node.x, node.y + node.height}
            };
            for (const auto& pt : c) b.include(node.transform.apply(pt));
            break;
        }
        case ShapeKind::Circle: {
            Vec2 c[4] = {
                {node.center.x - node.radius, node.center.y - node.radius},
                {node.center.x + node.radius, node.center.y - node.radius},
                {node.center.x + node.radius, node.center.y + node.radius},
                {node.center.x - node.radius, node.center.y + node.radius}
            };
            for (const auto& pt : c) b.include(node.transform.apply(pt));
            break;
        }
        case ShapeKind::Ellipse: {
            Vec2 c[4] = {
                {node.center.x - node.rx, node.center.y - node.ry},
                {node.center.x + node.rx, node.center.y - node.ry},
                {node.center.x + node.rx, node.center.y + node.ry},
                {node.center.x - node.rx, node.center.y + node.ry}
            };
            for (const auto& pt : c) b.include(node.transform.apply(pt));
            break;
        }
        case ShapeKind::Line: {
            b.include(node.transform.apply(node.from));
            b.include(node.transform.apply(node.to));
            break;
        }
        case ShapeKind::Polygon:
        case ShapeKind::Polyline: {
            node.point_lengths.resize(node.points.size(), 0.0);
            double acc = 0.0;
            for (size_t i = 0; i < node.points.size(); ++i) {
                if (i > 0) acc += (node.points[i] - node.points[i - 1]).length();
                node.point_lengths[i] = acc;
                b.include(node.transform.apply(node.points[i]));
            }
            break;
        }
        case ShapeKind::Spline: {
            node.path_segments = spline_to_cubics(node.points, node.tension, node.closed);
            node.subpaths = flatten_path_segments(node.path_segments);
            for (const auto& sp : node.subpaths) {
                for (const auto& pt : sp.points) b.include(node.transform.apply(pt));
            }
            break;
        }
        case ShapeKind::Path: {
            node.subpaths = flatten_path_segments(node.path_segments);
            for (const auto& sp : node.subpaths) {
                for (const auto& pt : sp.points) b.include(node.transform.apply(pt));
            }
            break;
        }
        case ShapeKind::Boolean:
        case ShapeKind::Group: {
            Transform2D parent_tf = node.transform;
            node.transform = Transform2D::identity();
            node.inv_transform = Transform2D::identity();
            node.scale_factor = 1.0;
            for (auto& ch : node.children) {
                if (!parent_tf.is_identity()) {
                    ch.transform = parent_tf.multiply(ch.transform);
                }
                compute_shape_bbox_and_register(ch, scene_reg);
                b.merge(ch.bbox);
            }
            break;
        }
        case ShapeKind::Text: {
            if (!node.text_subpaths.empty()) {
                for (const auto& sp : node.text_subpaths) {
                    for (const auto& pt : sp.points) b.include(node.transform.apply(pt));
                }
            } else {
                size_t glyphs = 0;
                for (unsigned char c : node.text_content) {
                    if ((c & 0xC0) != 0x80) ++glyphs;
                }
                double approx_w = static_cast<double>(glyphs) * (node.font_size * 0.62 + node.letter_spacing);
                double approx_h = node.font_size;
                double x0 = node.text_pos.x;
                if (node.text_align == TextAlign::Center) x0 -= approx_w * 0.5;
                else if (node.text_align == TextAlign::Right) x0 -= approx_w;
                double y0 = node.text_pos.y - approx_h * 0.5;
                b.include(node.transform.apply({x0, y0}));
                b.include(node.transform.apply({x0 + approx_w, y0 + approx_h}));
            }
            break;
        }
    }
    node.bbox = b;

    BBox rb = b;
    double pad = node.style.stroke_width + 4.0;
    if (node.style.arrow_mode != ArrowMode::None) {
        pad += node.style.arrow_size * 1.6;
    }
    if (node.style.shadow.enabled) {
        pad += std::max(std::abs(node.style.shadow.dx), std::abs(node.style.shadow.dy)) + node.style.shadow.blur * 1.5;
    }
    if (node.style.glow.enabled) {
        pad += node.style.glow.radius * 1.5;
    }
    if (node.kind == ShapeKind::Boolean && node.bool_op == BooleanOp::SmoothUnion) {
        pad += node.smoothness + 4.0;
    }
    rb.expand(pad);
    node.render_bounds = rb;

    if (scene_reg && !node.id.empty()) {
        scene_reg->node_bboxes[node.id] = node.bbox;
        scene_reg->named_nodes[node.id] = node;
    }
}

void recompute_scene_Caches(Scene& scene) {
    scene.node_bboxes.clear();
    scene.named_nodes.clear();
    for (auto& n : scene.nodes) {
        compute_shape_bbox_and_register(n, &scene);
    }
}

// ============================================================================
// 4. Lexer & Recursive-Descent Parser with Relational Constraint & Layout Solver
// ============================================================================

enum class TokKind {
    Eof,
    Ident,
    String,
    Number,
    HexColor,
    Ref,      // @id or @id.prop
    LBrace,   // {
    RBrace,   // }
    LParen,   // (
    RParen,   // )
    LBracket, // [
    RBracket, // ]
    Colon,    // :
    Comma,    // ,
    Semi,     // ;
    Assign,   // =
    Plus,     // +
    Minus,    // -
    Star,     // *
    Slash,    // /
    Percent   // %
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;
    double number = 0.0;
    int line = 1;
    int col = 1;
};

struct ParseError : public std::runtime_error {
    int line;
    int col;
    ParseError(const std::string& msg, int l, int c)
        : std::runtime_error(msg), line(l), col(c) {}
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos_ < src_.size()) {
            skip_whitespace_and_comments();
            if (pos_ >= src_.size()) break;

            int start_line = line_;
            int start_col = col_;
            char ch = src_[pos_];

            if (ch == '{') { advance(); tokens.push_back({TokKind::LBrace, "{", 0, start_line, start_col}); continue; }
            if (ch == '}') { advance(); tokens.push_back({TokKind::RBrace, "}", 0, start_line, start_col}); continue; }
            if (ch == '(') { advance(); tokens.push_back({TokKind::LParen, "(", 0, start_line, start_col}); continue; }
            if (ch == ')') { advance(); tokens.push_back({TokKind::RParen, ")", 0, start_line, start_col}); continue; }
            if (ch == '[') { advance(); tokens.push_back({TokKind::LBracket, "[", 0, start_line, start_col}); continue; }
            if (ch == ']') { advance(); tokens.push_back({TokKind::RBracket, "]", 0, start_line, start_col}); continue; }
            if (ch == ':') { advance(); tokens.push_back({TokKind::Colon, ":", 0, start_line, start_col}); continue; }
            if (ch == ',') { advance(); tokens.push_back({TokKind::Comma, ",", 0, start_line, start_col}); continue; }
            if (ch == ';') { advance(); tokens.push_back({TokKind::Semi, ";", 0, start_line, start_col}); continue; }
            if (ch == '=') { advance(); tokens.push_back({TokKind::Assign, "=", 0, start_line, start_col}); continue; }
            if (ch == '+') { advance(); tokens.push_back({TokKind::Plus, "+", 0, start_line, start_col}); continue; }
            if (ch == '*') { advance(); tokens.push_back({TokKind::Star, "*", 0, start_line, start_col}); continue; }
            if (ch == '/') { advance(); tokens.push_back({TokKind::Slash, "/", 0, start_line, start_col}); continue; }
            if (ch == '%') { advance(); tokens.push_back({TokKind::Percent, "%", 0, start_line, start_col}); continue; }
            if (ch == '-') {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '>') {
                    advance();
                    advance();
                    continue;
                }
                advance();
                tokens.push_back({TokKind::Minus, "-", 0, start_line, start_col});
                continue;
            }

            if (ch == '#') {
                std::string hex;
                advance(); // '#'
                while (pos_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_]))) {
                    hex.push_back(src_[pos_]);
                    advance();
                }
                if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) {
                    throw ParseError("Invalid hex color literal '#" + hex + "'", start_line, start_col);
                }
                tokens.push_back({TokKind::HexColor, "#" + hex, 0, start_line, start_col});
                continue;
            }

            if (ch == '@') {
                std::string ref;
                advance(); // '@'
                while (pos_ < src_.size() &&
                       (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' || src_[pos_] == '.' || src_[pos_] == '-')) {
                    ref.push_back(src_[pos_]);
                    advance();
                }
                if (ref.empty()) {
                    throw ParseError("Expected identifier after '@'", start_line, start_col);
                }
                tokens.push_back({TokKind::Ref, ref, 0, start_line, start_col});
                continue;
            }

            if (ch == '"' || ch == '\'') {
                char quote = ch;
                advance();
                std::string s;
                while (pos_ < src_.size() && src_[pos_] != quote) {
                    if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                        advance();
                        char esc = src_[pos_];
                        if (esc == 'n') s.push_back('\n');
                        else if (esc == 't') s.push_back('\t');
                        else s.push_back(esc);
                        advance();
                    } else {
                        s.push_back(src_[pos_]);
                        advance();
                    }
                }
                if (pos_ >= src_.size()) {
                    throw ParseError("Unterminated string literal", start_line, start_col);
                }
                advance(); // closing quote
                tokens.push_back({TokKind::String, s, 0, start_line, start_col});
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(ch)) ||
                (ch == '.' && pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
                std::string num_str;
                while (pos_ < src_.size() &&
                       (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) {
                    num_str.push_back(src_[pos_]);
                    advance();
                }
                double val = std::stod(num_str);
                if (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
                    std::string unit;
                    while (pos_ < src_.size() && std::isalpha(static_cast<unsigned char>(src_[pos_]))) {
                        unit.push_back(src_[pos_]);
                        advance();
                    }
                    if (unit == "rad") {
                        val = val * 180.0 / PI;
                    } else if (unit == "ms") {
                        val = val * 0.001;
                    }
                }
                tokens.push_back({TokKind::Number, num_str, val, start_line, start_col});
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                std::string id;
                while (pos_ < src_.size() &&
                       (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_' || src_[pos_] == '-' || src_[pos_] == '.')) {
                    id.push_back(src_[pos_]);
                    advance();
                }
                tokens.push_back({TokKind::Ident, id, 0, start_line, start_col});
                continue;
            }

            throw ParseError(std::string("Unexpected character '") + ch + "'", start_line, start_col);
        }
        tokens.push_back({TokKind::Eof, "", 0, line_, col_});
        return tokens;
    }

private:
    void advance() {
        if (pos_ < src_.size()) {
            if (src_[pos_] == '\n') {
                line_++;
                col_ = 1;
            } else {
                col_++;
            }
            pos_++;
        }
    }

    void skip_whitespace_and_comments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                advance();
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                advance();
                advance();
                while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    advance();
                }
                if (pos_ + 1 < src_.size()) {
                    advance();
                    advance();
                }
                continue;
            }
            break;
        }
    }

    const std::string& src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

class Parser {
public:
    Parser(const std::vector<Token>& tokens, Scene& scene)
        : tokens_(tokens), scene_(scene) {
        sync_canvas_vars();
    }

    void parse_document() {
        if (peek().kind == TokKind::Ident && peek().text == "mesh") {
            consume(); // 'mesh'
            if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
                scene_.name = consume().text;
            }
            expect(TokKind::LBrace, "Expected '{' after mesh header");
            while (!at_eof() && peek().kind != TokKind::RBrace) {
                parse_top_statement();
            }
            expect(TokKind::RBrace, "Expected '}' at end of mesh document");
        } else {
            while (!at_eof()) {
                parse_top_statement();
            }
        }
    }

private:
    const Token& peek(size_t offset = 0) const {
        size_t idx = std::min(idx_ + offset, tokens_.size() - 1);
        return tokens_[idx];
    }

    const Token& consume() {
        const Token& t = peek();
        if (idx_ + 1 < tokens_.size()) idx_++;
        return t;
    }

    bool at_eof() const { return peek().kind == TokKind::Eof; }

    void expect(TokKind k, const std::string& err_msg) {
        if (peek().kind != k) {
            throw ParseError(err_msg + " (got '" + peek().text + "')", peek().line, peek().col);
        }
        consume();
    }

    void skip_separators() {
        while (peek().kind == TokKind::Semi || peek().kind == TokKind::Comma) {
            consume();
        }
    }

    void sync_canvas_vars() {
        scene_.variables["width"] = static_cast<double>(scene_.width);
        scene_.variables["height"] = static_cast<double>(scene_.height);
        scene_.variables["min_dim"] = static_cast<double>(std::min(scene_.width, scene_.height));
        scene_.variables["max_dim"] = static_cast<double>(std::max(scene_.width, scene_.height));
        scene_.variables["PI"] = PI;
        scene_.variables["TAU"] = 2.0 * PI;
        scene_.variables["t"] = scene_.eval_time;
        scene_.variables["time"] = scene_.eval_time;
    }

    bool is_shape_keyword(const std::string& kw) const {
        return kw == "rect" || kw == "circle" || kw == "ellipse" || kw == "line" ||
               kw == "polygon" || kw == "polyline" || kw == "spline" || kw == "path" ||
               kw == "contour" || kw == "boolean" || kw == "text" || kw == "group" ||
               kw == "layer" || kw == "use" || kw == "repeat" || kw == "stack" || kw == "grid" ||
               kw == "star" || kw == "ngon" || kw == "regular_polygon" || kw == "hexagon" ||
               kw == "ring" || kw == "annulus" || kw == "arc" || kw == "sector";
    }

    bool is_color_start() const {
        if (peek().kind == TokKind::HexColor) return true;
        if (peek().kind == TokKind::Ref && scene_.color_variables.count(peek().text)) return true;
        if (peek().kind == TokKind::Ident) {
            const std::string& s = peek().text;
            if (s == "oklch" || s == "oklab" || s == "rgb" || s == "rgba" || s == "hsl" || s == "hsla" ||
                s == "mix" || s == "lighten" || s == "darken" || s == "fade" || s == "alpha" ||
                s == "white" || s == "black" || s == "transparent") {
                return true;
            }
            if (scene_.color_variables.count(s)) return true;
        }
        return false;
    }

    // ---- Expression & Relational Reference Evaluation ----

    bool is_point_ref(const std::string& ref_str, Vec2& out_pt) const {
        auto pit = scene_.point_variables.find(ref_str);
        if (pit != scene_.point_variables.end()) {
            out_pt = pit->second;
            return true;
        }
        auto dot_pos = ref_str.find('.');
        if (dot_pos == std::string::npos) return false;
        std::string id = ref_str.substr(0, dot_pos);
        std::string prop = ref_str.substr(dot_pos + 1);
        auto it = scene_.node_bboxes.find(id);
        if (it == scene_.node_bboxes.end()) {
            throw ParseError("Unknown node reference '@" + id + "'", peek().line, peek().col);
        }
        const BBox& b = it->second;
        if (prop == "center")        { out_pt = {b.center_x(), b.center_y()}; return true; }
        if (prop == "top_left")      { out_pt = {b.min_x, b.min_y}; return true; }
        if (prop == "top_right")     { out_pt = {b.max_x, b.min_y}; return true; }
        if (prop == "bottom_left")   { out_pt = {b.min_x, b.max_y}; return true; }
        if (prop == "bottom_right")  { out_pt = {b.max_x, b.max_y}; return true; }
        if (prop == "top_center")    { out_pt = {b.center_x(), b.min_y}; return true; }
        if (prop == "bottom_center") { out_pt = {b.center_x(), b.max_y}; return true; }
        if (prop == "left_center")   { out_pt = {b.min_x, b.center_y()}; return true; }
        if (prop == "right_center")  { out_pt = {b.max_x, b.center_y()}; return true; }
        return false;
    }

    double eval_scalar_ref(const Token& tok) const {
        const std::string& ref_str = tok.text;
        auto dot_pos = ref_str.find('.');
        if (dot_pos == std::string::npos) {
            auto vit = scene_.variables.find(ref_str);
            if (vit != scene_.variables.end()) return vit->second;
            throw ParseError("Unknown variable reference '@" + ref_str + "'", tok.line, tok.col);
        }
        std::string id = ref_str.substr(0, dot_pos);
        std::string prop = ref_str.substr(dot_pos + 1);
        auto pit = scene_.point_variables.find(id);
        if (pit != scene_.point_variables.end()) {
            if (prop == "x") return pit->second.x;
            if (prop == "y") return pit->second.y;
        }
        auto it = scene_.node_bboxes.find(id);
        if (it == scene_.node_bboxes.end()) {
            throw ParseError("Unknown node reference '@" + id + "'", tok.line, tok.col);
        }
        const BBox& b = it->second;
        if (prop == "x" || prop == "left")       return b.min_x;
        if (prop == "y" || prop == "top")        return b.min_y;
        if (prop == "right")                     return b.max_x;
        if (prop == "bottom")                    return b.max_y;
        if (prop == "width" || prop == "w")      return b.width();
        if (prop == "height" || prop == "h")     return b.height();
        if (prop == "center_x" || prop == "cx")  return b.center_x();
        if (prop == "center_y" || prop == "cy")  return b.center_y();
        if (prop == "radius" || prop == "r")     return 0.5 * std::min(b.width(), b.height());
        throw ParseError("Unknown scalar property '." + prop + "' on '@" + id + "'", tok.line, tok.col);
    }

    double parse_expr() {
        double v = parse_term();
        while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
            TokKind op = consume().kind;
            double rhs = parse_term();
            v = (op == TokKind::Plus) ? (v + rhs) : (v - rhs);
        }
        return v;
    }

    double parse_term() {
        double v = parse_unary();
        while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash || peek().kind == TokKind::Percent) {
            TokKind op = consume().kind;
            double rhs = parse_unary();
            if (op == TokKind::Star) v *= rhs;
            else if (op == TokKind::Slash) v = (std::abs(rhs) > 1e-12) ? (v / rhs) : 0.0;
            else v = (std::abs(rhs) > 1e-12) ? std::fmod(v, rhs) : 0.0;
        }
        return v;
    }

    double parse_unary() {
        if (peek().kind == TokKind::Minus) {
            consume();
            return -parse_unary();
        }
        if (peek().kind == TokKind::Plus) {
            consume();
            return parse_unary();
        }
        return parse_primary();
    }

    double parse_primary() {
        const Token& t = peek();
        if (t.kind == TokKind::Number) {
            consume();
            double val = t.number;
            if (peek().kind == TokKind::Percent) {
                consume();
                val *= 0.01;
            }
            return val;
        }
        if (t.kind == TokKind::Ref) {
            consume();
            return eval_scalar_ref(t);
        }
        if (t.kind == TokKind::LParen) {
            consume();
            double v = parse_expr();
            expect(TokKind::RParen, "Expected ')' in expression");
            return v;
        }
        if (t.kind == TokKind::Ident) {
            std::string name = consume().text;
            if (name == "true") return 1.0;
            if (name == "false") return 0.0;
            if (peek().kind == TokKind::LParen) {
                consume(); // '('
                std::vector<double> args;
                if (peek().kind != TokKind::RParen) {
                    args.push_back(parse_expr());
                    while (peek().kind == TokKind::Comma) {
                        consume();
                        args.push_back(parse_expr());
                    }
                }
                expect(TokKind::RParen, "Expected ')' after function arguments");
                auto req = [&](size_t n) {
                    if (args.size() < n) {
                        throw ParseError("Function '" + name + "' requires " + std::to_string(n) + " arguments", t.line, t.col);
                    }
                };
                if (name == "sin")     { req(1); return std::sin(args[0] * PI / 180.0); }
                if (name == "cos")     { req(1); return std::cos(args[0] * PI / 180.0); }
                if (name == "tan")     { req(1); return std::tan(args[0] * PI / 180.0); }
                if (name == "sin_rad") { req(1); return std::sin(args[0]); }
                if (name == "cos_rad") { req(1); return std::cos(args[0]); }
                if (name == "sqrt")    { req(1); return std::sqrt(std::max(0.0, args[0])); }
                if (name == "abs")     { req(1); return std::abs(args[0]); }
                if (name == "floor")   { req(1); return std::floor(args[0]); }
                if (name == "ceil")    { req(1); return std::ceil(args[0]); }
                if (name == "round")   { req(1); return std::round(args[0]); }
                if (name == "fract")   { req(1); return args[0] - std::floor(args[0]); }
                if (name == "mod")     { req(2); return std::abs(args[1]) > 1e-12 ? std::fmod(args[0], args[1]) : 0.0; }
                if (name == "min")     { req(2); return std::min(args[0], args[1]); }
                if (name == "max")     { req(2); return std::max(args[0], args[1]); }
                if (name == "pow")     { req(2); return std::pow(args[0], args[1]); }
                if (name == "clamp")   { req(3); return std::clamp(args[0], args[1], args[2]); }
                if (name == "lerp")    { req(3); return args[0] + (args[1] - args[0]) * args[2]; }
                if (name == "smoothstep") {
                    req(3);
                    double u = clamp01((args[2] - args[0]) / std::max(1e-9, args[1] - args[0]));
                    return u * u * (3.0 - 2.0 * u);
                }
                if (name == "polar_x") { req(2); return args[0] * std::cos(args[1] * PI / 180.0); }
                if (name == "polar_y") { req(2); return args[0] * std::sin(args[1] * PI / 180.0); }
                if (name == "noise")   {
                    req(2);
                    double seed = args.size() >= 3 ? args[2] : 0.0;
                    return fbm_noise_2d(args[0], args[1], 4, 0.5, 2.0, seed);
                }

                // Declarative Temporal, Waveform & Spring Physics Functions
                if (name == "oscillate") {
                    req(4);
                    double phase_rad = (args.size() >= 5 ? args[4] : 0.0) * PI / 180.0;
                    double s = 0.5 + 0.5 * std::sin(2.0 * PI * args[1] * args[0] + phase_rad);
                    return args[2] + (args[3] - args[2]) * s;
                }
                if (name == "wave") {
                    req(2);
                    double phase_rad = (args.size() >= 3 ? args[2] : 0.0) * PI / 180.0;
                    return std::sin(2.0 * PI * args[1] * args[0] + phase_rad);
                }
                if (name == "pulse") {
                    req(2);
                    return 0.5 - 0.5 * std::cos(2.0 * PI * args[1] * args[0]);
                }
                if (name == "saw") {
                    req(2);
                    double ph = args.size() >= 3 ? args[2] : 0.0;
                    double v = args[0] * args[1] + ph;
                    return v - std::floor(v);
                }
                if (name == "ping_pong") {
                    req(2);
                    double v = args[0] * args[1];
                    double f = v - std::floor(v);
                    return 1.0 - std::abs(2.0 * f - 1.0);
                }
                if (name == "ease_in_out") {
                    req(5);
                    double dur = std::max(1e-6, args[2]);
                    double u = clamp01((args[0] - args[1]) / dur);
                    double s = u * u * (3.0 - 2.0 * u);
                    return args[3] + (args[4] - args[3]) * s;
                }
                if (name == "ease_out_cubic") {
                    req(5);
                    double dur = std::max(1e-6, args[2]);
                    double u = clamp01((args[0] - args[1]) / dur);
                    double inv = 1.0 - u;
                    double s = 1.0 - inv * inv * inv;
                    return args[3] + (args[4] - args[3]) * s;
                }
                if (name == "ease_out_back") {
                    req(5);
                    double dur = std::max(1e-6, args[2]);
                    double u = clamp01((args[0] - args[1]) / dur);
                    double c1 = args.size() >= 6 ? args[5] : 1.70158;
                    double c3 = c1 + 1.0;
                    double um1 = u - 1.0;
                    double s = 1.0 + c3 * um1 * um1 * um1 + c1 * um1 * um1;
                    return args[3] + (args[4] - args[3]) * s;
                }
                if (name == "spring") {
                    req(4);
                    double dt = args[0] - args[1];
                    if (dt <= 0.0) return args[2];
                    double k = args.size() >= 5 ? std::max(1.0, args[4]) : 180.0;
                    double c = args.size() >= 6 ? std::max(0.1, args[5]) : 14.0;
                    double w0 = std::sqrt(k);
                    double zeta = std::min(0.99, c / (2.0 * w0));
                    double wd = w0 * std::sqrt(1.0 - zeta * zeta);
                    double env = std::exp(-zeta * w0 * dt);
                    double osc = std::cos(wd * dt) + (zeta * w0 / wd) * std::sin(wd * dt);
                    return args[3] - (args[3] - args[2]) * env * osc;
                }
                throw ParseError("Unknown math/animation function '" + name + "'", t.line, t.col);
            }
            auto vit = scene_.variables.find(name);
            if (vit != scene_.variables.end()) return vit->second;
            if (name.find('.') != std::string::npos) {
                return eval_scalar_ref(t);
            }
            throw ParseError("Unknown identifier '" + name + "' in expression", t.line, t.col);
        }
        throw ParseError("Expected expression at '" + t.text + "'", t.line, t.col);
    }

    Vec2 parse_single_vec2() {
        if (peek().kind == TokKind::Ref) {
            Vec2 pt;
            if (is_point_ref(peek().text, pt)) {
                consume();
                return pt;
            }
        }
        if (peek().kind == TokKind::Ident) {
            auto pit = scene_.point_variables.find(peek().text);
            if (pit != scene_.point_variables.end()) {
                consume();
                return pit->second;
            }
            if (peek().text.find('.') != std::string::npos) {
                Vec2 pt;
                if (is_point_ref(peek().text, pt)) {
                    consume();
                    return pt;
                }
            }
        }
        if (peek().kind == TokKind::LParen) {
            consume(); // '('
            double x = parse_expr();
            expect(TokKind::Comma, "Expected ',' inside 2D point tuple (x, y)");
            double y = parse_expr();
            expect(TokKind::RParen, "Expected ')' after 2D point tuple (x, y)");
            return {x, y};
        }
        double x = parse_expr();
        expect(TokKind::Comma, "Expected ',' for 2D coordinate pair");
        double y = parse_expr();
        return {x, y};
    }

    Vec2 parse_vec2() {
        Vec2 v = parse_single_vec2();
        while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
            TokKind op = consume().kind;
            Vec2 delta = parse_single_vec2();
            v = (op == TokKind::Plus) ? (v + delta) : (v - delta);
        }
        return v;
    }

    Color parse_color() {
        const Token& t = peek();
        if (t.kind == TokKind::HexColor) {
            consume();
            return Color::from_hex(t.text);
        }
        if (t.kind == TokKind::Ref) {
            auto cit = scene_.color_variables.find(t.text);
            if (cit != scene_.color_variables.end()) {
                consume();
                return cit->second;
            }
        }
        if (t.kind == TokKind::Ident) {
            std::string fn = consume().text;
            if (fn == "transparent" || fn == "none") return Color::from_srgb(0, 0, 0, 0);
            if (fn == "white") return Color::from_srgb(1, 1, 1, 1);
            if (fn == "black") return Color::from_srgb(0, 0, 0, 1);
            if (peek().kind != TokKind::LParen) {
                auto cit = scene_.color_variables.find(fn);
                if (cit != scene_.color_variables.end()) return cit->second;
                throw ParseError("Unknown color variable '" + fn + "'", t.line, t.col);
            }
            expect(TokKind::LParen, "Expected '(' after color function '" + fn + "'");

            if (fn == "mix") {
                Color c0 = parse_color();
                expect(TokKind::Comma, "Expected ',' after first color in mix(c0, c1, t)");
                Color c1 = parse_color();
                expect(TokKind::Comma, "Expected ',' after second color in mix(c0, c1, t)");
                double ratio = parse_expr();
                expect(TokKind::RParen, "Expected ')' after mix(c0, c1, t)");
                return Color::lerp_oklab(c0, c1, ratio);
            }
            if (fn == "lighten") {
                Color c0 = parse_color();
                expect(TokKind::Comma, "Expected ',' after color in lighten(color, amount)");
                double amt = parse_expr();
                expect(TokKind::RParen, "Expected ')' after lighten(color, amount)");
                return c0.lighten(amt);
            }
            if (fn == "darken") {
                Color c0 = parse_color();
                expect(TokKind::Comma, "Expected ',' after color in darken(color, amount)");
                double amt = parse_expr();
                expect(TokKind::RParen, "Expected ')' after darken(color, amount)");
                return c0.darken(amt);
            }
            if (fn == "fade" || fn == "alpha") {
                Color c0 = parse_color();
                expect(TokKind::Comma, "Expected ',' after color in fade(color, alpha)");
                double a = parse_expr();
                expect(TokKind::RParen, "Expected ')' after fade(color, alpha)");
                return c0.with_alpha(a);
            }

            std::vector<double> args;
            args.push_back(parse_expr());
            while (peek().kind == TokKind::Comma) {
                consume();
                args.push_back(parse_expr());
            }
            expect(TokKind::RParen, "Expected ')' after color arguments");
            if (fn == "oklch") {
                if (args.size() < 3) throw ParseError("oklch(L, C, H, [alpha]) requires at least 3 arguments", t.line, t.col);
                double L = args[0] > 1.0 ? args[0] / 100.0 : args[0];
                double C = args[1];
                double H = args[2];
                double A = args.size() >= 4 ? args[3] : 1.0;
                return Color::from_oklch(L, C, H, A);
            }
            if (fn == "oklab") {
                if (args.size() < 3) throw ParseError("oklab(L, a, b, [alpha]) requires at least 3 arguments", t.line, t.col);
                double A = args.size() >= 4 ? args[3] : 1.0;
                return Color::from_oklab(args[0], args[1], args[2], A);
            }
            if (fn == "rgb" || fn == "rgba") {
                if (args.size() < 3) throw ParseError("rgb/rgba requires at least 3 arguments", t.line, t.col);
                double scale = (args[0] > 1.0 || args[1] > 1.0 || args[2] > 1.0) ? (1.0 / 255.0) : 1.0;
                double A = args.size() >= 4 ? args[3] : 1.0;
                return Color::from_srgb(args[0] * scale, args[1] * scale, args[2] * scale, A);
            }
            if (fn == "hsl" || fn == "hsla") {
                if (args.size() < 3) throw ParseError("hsl/hsla requires at least 3 arguments", t.line, t.col);
                double s = args[1] > 1.0 ? args[1] / 100.0 : args[1];
                double l = args[2] > 1.0 ? args[2] / 100.0 : args[2];
                double A = args.size() >= 4 ? args[3] : 1.0;
                return Color::from_hsl(args[0], s, l, A);
            }
            throw ParseError("Unknown color constructor '" + fn + "'", t.line, t.col);
        }
        throw ParseError("Expected color literal or function at '" + t.text + "'", t.line, t.col);
    }

    PaintServer parse_paint_value() {
        if (peek().kind == TokKind::Ident && peek().text == "none") {
            consume();
            PaintServer ps;
            ps.kind = PaintKind::None;
            return ps;
        }
        if (peek().kind == TokKind::Ref) {
            std::string ref_id = peek().text;
            auto it = scene_.paint_servers.find(ref_id);
            if (it != scene_.paint_servers.end()) {
                consume();
                return it->second;
            }
            auto cit = scene_.color_variables.find(ref_id);
            if (cit != scene_.color_variables.end()) {
                consume();
                PaintServer ps;
                ps.kind = PaintKind::Solid;
                ps.solid_color = cit->second;
                return ps;
            }
            throw ParseError("Unknown gradient or color reference '@" + ref_id + "'", peek().line, peek().col);
        }
        PaintServer ps;
        ps.kind = PaintKind::Solid;
        ps.solid_color = parse_color();
        return ps;
    }

    void parse_shadow_args(ShadowSpec& sh) {
        if (peek().kind == TokKind::Colon) consume();
        expect(TokKind::LParen, "Expected '(' for shadow(dx, dy, blur, color)");
        sh.enabled = true;
        sh.dx = parse_expr();
        expect(TokKind::Comma, "Expected ',' after shadow dx");
        sh.dy = parse_expr();
        expect(TokKind::Comma, "Expected ',' after shadow dy");
        sh.blur = parse_expr();
        expect(TokKind::Comma, "Expected ',' after shadow blur");
        sh.color = parse_color();
        expect(TokKind::RParen, "Expected ')' after shadow specification");
    }

    void parse_glow_args(GlowSpec& gl) {
        if (peek().kind == TokKind::Colon) consume();
        expect(TokKind::LParen, "Expected '(' for glow(radius, color)");
        gl.enabled = true;
        gl.radius = parse_expr();
        expect(TokKind::Comma, "Expected ',' after glow radius");
        gl.color = parse_color();
        expect(TokKind::RParen, "Expected ')' after glow specification");
    }

    void parse_let_decl() {
        consume(); // 'let'
        if (peek().kind != TokKind::Ident) {
            throw ParseError("Expected variable name after 'let'", peek().line, peek().col);
        }
        std::string var_name = consume().text;
        expect(TokKind::Assign, "Expected '=' after variable name '" + var_name + "'");

        if (is_color_start()) {
            scene_.color_variables[var_name] = parse_color();
        } else {
            bool is_pt = false;
            if (peek().kind == TokKind::Ref) {
                Vec2 dummy;
                if (is_point_ref(peek().text, dummy)) is_pt = true;
            } else if (peek().kind == TokKind::Ident) {
                if (scene_.point_variables.count(peek().text)) {
                    is_pt = true;
                } else if (peek().text.find('.') != std::string::npos) {
                    Vec2 dummy;
                    if (is_point_ref(peek().text, dummy)) is_pt = true;
                }
            } else if (peek().kind == TokKind::LParen) {
                int depth = 0;
                for (size_t k = idx_; k < tokens_.size(); ++k) {
                    if (tokens_[k].kind == TokKind::LParen) depth++;
                    else if (tokens_[k].kind == TokKind::RParen) {
                        depth--;
                        if (depth == 0) break;
                    } else if (tokens_[k].kind == TokKind::Comma && depth == 1) {
                        is_pt = true;
                        break;
                    }
                }
            }
            if (is_pt) {
                Vec2 pt = parse_vec2();
                scene_.point_variables[var_name] = pt;
                scene_.variables[var_name + ".x"] = pt.x;
                scene_.variables[var_name + ".y"] = pt.y;
            } else {
                scene_.variables[var_name] = parse_expr();
            }
        }
        skip_separators();
    }

    // ---- Top-Level & Block Statements ----

    void parse_top_statement() {
        skip_separators();
        if (at_eof() || peek().kind == TokKind::RBrace) return;

        const Token& t = peek();
        if (t.kind != TokKind::Ident) {
            throw ParseError("Expected declaration keyword, got '" + t.text + "'", t.line, t.col);
        }
        std::string kw = t.text;

        if (kw == "canvas") {
            consume();
            expect(TokKind::LBrace, "Expected '{' after canvas");
            while (!at_eof() && peek().kind != TokKind::RBrace) {
                skip_separators();
                if (peek().kind == TokKind::RBrace) break;
                std::string prop = consume().text;
                expect(TokKind::Colon, "Expected ':' after canvas property '" + prop + "'");
                if (prop == "width") {
                    scene_.width = static_cast<uint32_t>(std::max(1.0, std::round(parse_expr())));
                    sync_canvas_vars();
                } else if (prop == "height") {
                    scene_.height = static_cast<uint32_t>(std::max(1.0, std::round(parse_expr())));
                    sync_canvas_vars();
                } else if (prop == "size") {
                    Vec2 sz = parse_vec2();
                    scene_.width = static_cast<uint32_t>(std::max(1.0, std::round(sz.x)));
                    scene_.height = static_cast<uint32_t>(std::max(1.0, std::round(sz.y)));
                    sync_canvas_vars();
                } else if (prop == "background" || prop == "bg") {
                    scene_.background = parse_color();
                    scene_.has_background = true;
                } else if (prop == "font_family" || prop == "font") {
                    scene_.default_font_family = parse_font_string_value();
                } else if (prop == "font_fallback") {
                    scene_.default_font_fallback = parse_font_string_value();
                } else {
                    throw ParseError("Unknown canvas property '" + prop + "'", peek().line, peek().col);
                }
                skip_separators();
            }
            expect(TokKind::RBrace, "Expected '}' after canvas block");
            return;
        }

        if (kw == "let") {
            parse_let_decl();
            return;
        }

        if (kw == "font" || kw == "font_face" || kw == "typeface") {
            parse_font_decl();
            return;
        }

        if (kw == "def" || kw == "component" || kw == "symbol") {
            parse_def_decl();
            return;
        }

        if (kw == "linear_gradient" || kw == "radial_gradient" || kw == "conic_gradient" ||
            kw == "mesh_gradient" || kw == "diffusion_gradient" || kw == "noise_gradient") {
            parse_gradient_decl();
            return;
        }

        ShapeNode node = parse_shape_decl();
        compute_shape_bbox_and_register(node, &scene_);
        scene_.nodes.push_back(std::move(node));
    }

    std::string parse_font_string_value() {
        // Accept a bare string/identifier or url("...") / url('...')
        if (peek().kind == TokKind::Ident && peek().text == "url" && peek(1).kind == TokKind::LParen) {
            consume(); // url
            consume(); // (
            if (peek().kind != TokKind::String && peek().kind != TokKind::Ident) {
                throw ParseError("Expected font URL string inside url(...)", peek().line, peek().col);
            }
            std::string value = consume().text;
            expect(TokKind::RParen, "Expected ')' after font URL");
            return value;
        }
        if (peek().kind != TokKind::String && peek().kind != TokKind::Ident) {
            throw ParseError("Expected a string value for font property", peek().line, peek().col);
        }
        return consume().text;
    }

    static double font_weight_from_name(const std::string& name) {
        if (name == "thin" || name == "hairline") return 100.0;
        if (name == "extralight" || name == "ultralight") return 200.0;
        if (name == "light") return 300.0;
        if (name == "regular" || name == "normal" || name == "book") return 400.0;
        if (name == "medium") return 500.0;
        if (name == "semibold" || name == "demibold") return 600.0;
        if (name == "bold") return 700.0;
        if (name == "extrabold" || name == "ultrabold") return 800.0;
        if (name == "black" || name == "heavy") return 900.0;
        return 400.0;
    }

    static FontStyle font_style_from_name(const std::string& name) {
        if (name == "italic" || name == "cursive") return FontStyle::Italic;
        if (name == "oblique" || name == "slanted") return FontStyle::Oblique;
        return FontStyle::Normal;
    }

    static double font_stretch_from_name(const std::string& name) {
        if (name == "ultracondensed") return 50.0;
        if (name == "extracondensed") return 62.5;
        if (name == "condensed" || name == "narrow") return 75.0;
        if (name == "semiexpanded") return 112.5;
        if (name == "expanded" || name == "wide") return 125.0;
        if (name == "extraexpanded") return 150.0;
        if (name == "ultraexpanded") return 200.0;
        return 100.0;
    }

    void parse_font_decl() {
        consume(); // font / font_face / typeface
        if (peek().kind != TokKind::String && peek().kind != TokKind::Ident) {
            throw ParseError("Expected a font identifier after 'font'", peek().line, peek().col);
        }
        FontSpec spec;
        spec.id = consume().text;
        expect(TokKind::LBrace, "Expected '{' after font identifier");

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;
            std::string prop = consume().text;
            expect(TokKind::Colon, "Expected ':' after font property '" + prop + "'");
            if (prop == "family" || prop == "name" || prop == "font_family") {
                spec.family = parse_font_string_value();
            } else if (prop == "src" || prop == "path" || prop == "file" || prop == "source") {
                spec.src = parse_font_string_value();
            } else if (prop == "fallback" || prop == "fallback_family") {
                spec.fallback = parse_font_string_value();
            } else if (prop == "weight" || prop == "font_weight") {
                if (peek().kind == TokKind::Ident && peek().text != "true" && peek().text != "false") {
                    spec.weight = font_weight_from_name(consume().text);
                } else {
                    spec.weight = parse_expr();
                }
            } else if (prop == "style" || prop == "font_style" || prop == "slant") {
                if (peek().kind == TokKind::Ident) {
                    spec.style = font_style_from_name(consume().text);
                } else {
                    int s = static_cast<int>(std::round(parse_expr()));
                    spec.style = (s == 1) ? FontStyle::Italic : (s == 2 ? FontStyle::Oblique : FontStyle::Normal);
                }
            } else if (prop == "stretch" || prop == "width" || prop == "font_stretch") {
                if (peek().kind == TokKind::Ident) {
                    spec.stretch = font_stretch_from_name(consume().text);
                } else {
                    spec.stretch = parse_expr();
                }
            } else if (prop == "index" || prop == "face_index") {
                spec.face_index = static_cast<int>(std::round(parse_expr()));
            } else {
                throw ParseError("Unknown font property '" + prop + "'", peek().line, peek().col);
            }
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after font declaration");
        if (spec.family.empty() && spec.src.empty()) {
            throw ParseError("Font '" + spec.id + "' needs either a family or a src path", peek().line, peek().col);
        }
        scene_.fonts[spec.id] = spec;
    }

    void parse_def_decl() {
        consume(); // 'def'
        if (peek().kind != TokKind::String && peek().kind != TokKind::Ident) {
            throw ParseError("Expected component name after 'def'", peek().line, peek().col);
        }
        std::string def_name = consume().text;
        expect(TokKind::LBrace, "Expected '{' after def '" + def_name + "'");

        ShapeNode tpl;
        tpl.kind = ShapeKind::Group;
        tpl.id = def_name;

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;
            if (peek().kind == TokKind::Ident && peek().text == "let") {
                parse_let_decl();
                continue;
            }
            ShapeNode child = parse_shape_decl();
            compute_shape_bbox_and_register(child, nullptr);
            tpl.children.push_back(std::move(child));
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after def '" + def_name + "'");
        compute_shape_bbox_and_register(tpl, nullptr);
        scene_.defs[def_name] = std::move(tpl);
    }

    ShapeNode parse_use_decl() {
        const Token& kw_tok = consume(); // 'use'
        std::string inst_id;
        if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
            inst_id = consume().text;
        }
        expect(TokKind::LBrace, "Expected '{' after use declaration");

        std::string target_id;
        Vec2 translate_pos = {0, 0};
        Vec2 scale_vec = {1, 1};
        double rotate_deg = 0.0;
        double opacity = 1.0;
        BlendMode blend = BlendMode::Normal;
        std::string clip_id;

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;
            std::string prop = consume().text;
            expect(TokKind::Colon, "Expected ':' after property '" + prop + "' in use");
            if (prop == "target" || prop == "href" || prop == "component") {
                if (peek().kind == TokKind::Ref || peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
                    target_id = consume().text;
                } else {
                    throw ParseError("Expected @component_name for 'target' in use", peek().line, peek().col);
                }
            } else if (prop == "at" || prop == "pos" || prop == "translate") {
                translate_pos = parse_vec2();
            } else if (prop == "x") {
                translate_pos.x = parse_expr();
            } else if (prop == "y") {
                translate_pos.y = parse_expr();
            } else if (prop == "scale") {
                if (peek().kind == TokKind::LParen) scale_vec = parse_vec2();
                else { double s = parse_expr(); scale_vec = {s, s}; }
            } else if (prop == "rotate") {
                rotate_deg = parse_expr();
            } else if (prop == "opacity" || prop == "alpha") {
                opacity = clamp01(parse_expr());
            } else if (prop == "blend") {
                blend = parse_blend_mode();
            } else if (prop == "clip") {
                clip_id = consume().text;
            } else {
                throw ParseError("Unknown property '" + prop + "' in use block", peek().line, peek().col);
            }
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after use block");

        auto it = scene_.defs.find(target_id);
        if (it == scene_.defs.end()) {
            throw ParseError("Unknown component definition '@" + target_id + "' referenced in use", kw_tok.line, kw_tok.col);
        }

        ShapeNode inst = it->second;
        inst.id = inst_id;
        inst.style.opacity = opacity;
        inst.style.blend_mode = blend;
        inst.style.clip_id = clip_id;
        for (auto& ch : inst.children) {
            ch.id.clear();
            ch.style.opacity *= opacity;
            if (blend != BlendMode::Normal) ch.style.blend_mode = blend;
            if (!clip_id.empty() && ch.style.clip_id.empty()) ch.style.clip_id = clip_id;
        }

        Transform2D tf = Transform2D::translate(translate_pos.x, translate_pos.y)
                             .multiply(Transform2D::rotate_deg(rotate_deg))
                             .multiply(Transform2D::scale(scale_vec.x, scale_vec.y));
        inst.transform = inst.transform.multiply(tf);
        return inst;
    }

    ShapeNode parse_repeat_decl() {
        consume(); // 'repeat'
        std::string rep_id;
        if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
            rep_id = consume().text;
        }
        expect(TokKind::LBrace, "Expected '{' after repeat");

        int count = 2;
        Vec2 pivot = {0, 0};
        double step_rotate = 0.0;
        Vec2 step_translate = {0, 0};
        Vec2 step_scale = {1, 1};
        BlendMode blend = BlendMode::Normal;
        std::string clip_id;

        struct RepeatBodyItem {
            bool is_let = false;
            size_t token_idx = 0;
        };
        std::vector<RepeatBodyItem> body_items;

        bool had_prev_i = (scene_.variables.count("i") > 0);
        double prev_i = had_prev_i ? scene_.variables["i"] : 0.0;
        bool had_prev_rep_t = (scene_.variables.count("rep_t") > 0);
        double prev_rep_t = had_prev_rep_t ? scene_.variables["rep_t"] : 0.0;
        scene_.variables["i"] = 0.0;
        scene_.variables["rep_t"] = 0.0;

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;

            if (peek().kind == TokKind::Ident && peek().text == "let") {
                body_items.push_back({true, idx_});
                parse_let_decl();
                continue;
            }

            if (peek().kind == TokKind::Ident && is_shape_keyword(peek().text)) {
                body_items.push_back({false, idx_});
                (void)parse_shape_decl();
                skip_separators();
                continue;
            }

            std::string prop = consume().text;
            expect(TokKind::Colon, "Expected ':' after repeat property '" + prop + "'");
            if (prop == "count" || prop == "steps") {
                count = std::clamp(static_cast<int>(std::round(parse_expr())), 1, 1024);
            } else if (prop == "pivot" || prop == "origin" || prop == "center") {
                pivot = parse_vec2();
            } else if (prop == "step_rotate" || prop == "rotate_step") {
                step_rotate = parse_expr();
            } else if (prop == "step_translate" || prop == "offset" || prop == "step") {
                step_translate = parse_vec2();
            } else if (prop == "step_scale") {
                if (peek().kind == TokKind::LParen) step_scale = parse_vec2();
                else { double s = parse_expr(); step_scale = {s, s}; }
            } else if (prop == "blend") {
                blend = parse_blend_mode();
            } else if (prop == "clip") {
                clip_id = consume().text;
            } else {
                throw ParseError("Unknown property '" + prop + "' in repeat block", peek().line, peek().col);
            }
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after repeat block");
        size_t after_repeat_idx = idx_;

        ShapeNode group;
        group.kind = ShapeKind::Group;
        group.id = rep_id;
        group.style.blend_mode = blend;
        group.style.clip_id = clip_id;

        for (int i = 0; i < count; ++i) {
            double fi = static_cast<double>(i);
            scene_.variables["i"] = fi;
            scene_.variables["rep_t"] = (count > 1) ? (fi / static_cast<double>(count - 1)) : 0.0;

            Transform2D iter_tf = Transform2D::translate(step_translate.x * fi, step_translate.y * fi)
                                      .multiply(Transform2D::rotate_deg(step_rotate * fi, pivot))
                                      .multiply(Transform2D::scale(std::pow(step_scale.x, fi), std::pow(step_scale.y, fi), pivot));

            for (const auto& item : body_items) {
                idx_ = item.token_idx;
                if (item.is_let) {
                    parse_let_decl();
                } else {
                    ShapeNode clone = parse_shape_decl();
                    if (!clone.id.empty()) {
                        clone.id = clone.id + "_" + std::to_string(i);
                    }
                    if (blend != BlendMode::Normal && clone.style.blend_mode == BlendMode::Normal) {
                        clone.style.blend_mode = blend;
                    }
                    if (!clip_id.empty() && clone.style.clip_id.empty()) {
                        clone.style.clip_id = clip_id;
                    }
                    clone.transform = iter_tf.multiply(clone.transform);
                    group.children.push_back(std::move(clone));
                }
            }
        }

        idx_ = after_repeat_idx;
        if (had_prev_i) scene_.variables["i"] = prev_i;
        else scene_.variables.erase("i");
        if (had_prev_rep_t) scene_.variables["rep_t"] = prev_rep_t;
        else scene_.variables.erase("rep_t");

        return group;
    }

    // Declarative Auto-Layout Container (`stack` / `grid`)
    ShapeNode parse_stack_decl() {
        std::string kw = consume().text; // 'stack' or 'grid'
        std::string stack_id;
        if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
            stack_id = consume().text;
        }
        expect(TokKind::LBrace, "Expected '{' after " + kw);

        double start_x = 0.0;
        double start_y = 0.0;
        double gap_x = 16.0;
        double gap_y = 16.0;
        double padding = 0.0;
        int cols = 3;
        std::string dir = (kw == "grid") ? "grid" : "horizontal";
        std::string align = "start";
        Style container_style;
        double container_radius = 0.0;
        std::vector<ShapeNode> items;

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;

            if (peek().kind == TokKind::Ident && peek().text == "let") {
                parse_let_decl();
                continue;
            }

            if (peek().kind == TokKind::Ident && is_shape_keyword(peek().text)) {
                ShapeNode item = parse_shape_decl();
                compute_shape_bbox_and_register(item, nullptr);
                items.push_back(std::move(item));
                skip_separators();
                continue;
            }

            std::string prop = consume().text;
            if (prop == "shadow") {
                parse_shadow_args(container_style.shadow);
                skip_separators();
                continue;
            }
            if (prop == "glow") {
                parse_glow_args(container_style.glow);
                skip_separators();
                continue;
            }

            expect(TokKind::Colon, "Expected ':' after " + kw + " property '" + prop + "'");
            if (prop == "x") start_x = parse_expr();
            else if (prop == "y") start_y = parse_expr();
            else if (prop == "pos" || prop == "at" || prop == "top_left") {
                Vec2 p = parse_vec2();
                start_x = p.x;
                start_y = p.y;
            } else if (prop == "gap" || prop == "spacing") {
                if (peek().kind == TokKind::LParen) {
                    Vec2 g = parse_vec2();
                    gap_x = g.x;
                    gap_y = g.y;
                } else {
                    gap_x = gap_y = parse_expr();
                }
            } else if (prop == "padding" || prop == "pad") {
                padding = parse_expr();
            } else if (prop == "cols" || prop == "columns") {
                cols = std::max(1, static_cast<int>(std::round(parse_expr())));
            } else if (prop == "direction" || prop == "dir" || prop == "layout") {
                dir = consume().text;
            } else if (prop == "align") {
                align = consume().text;
            } else if (prop == "fill") {
                container_style.fill = parse_paint_value();
                if (container_style.fill.grain > 0.0 && container_style.grain <= 0.0) {
                    container_style.grain = container_style.fill.grain;
                }
            } else if (prop == "stroke") {
                container_style.stroke = parse_paint_value();
                if (container_style.stroke_width <= 0.0 && container_style.stroke.kind != PaintKind::None) {
                    container_style.stroke_width = 1.5;
                }
            } else if (prop == "stroke_width" || prop == "weight") {
                container_style.stroke_width = parse_expr();
            } else if (prop == "stroke_align") {
                std::string sa = consume().text;
                if (sa == "inner" || sa == "inside") container_style.stroke_align = StrokeAlign::Inner;
                else if (sa == "outer" || sa == "outside") container_style.stroke_align = StrokeAlign::Outer;
                else container_style.stroke_align = StrokeAlign::Center;
            } else if (prop == "radius" || prop == "r") {
                container_radius = parse_expr();
            } else if (prop == "opacity" || prop == "alpha") {
                container_style.opacity = clamp01(parse_expr());
            } else if (prop == "grain") {
                container_style.grain = clamp01(parse_expr());
            } else {
                throw ParseError("Unknown property '" + prop + "' in " + kw + " block", peek().line, peek().col);
            }
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after " + kw + " block");

        ShapeNode group;
        group.kind = ShapeKind::Group;
        group.id = stack_id;

        double cur_x = start_x + padding;
        double cur_y = start_y + padding;
        double max_cross = 0.0;
        for (const auto& it : items) {
            if (dir == "horizontal" || dir == "row") {
                max_cross = std::max(max_cross, it.bbox.height());
            } else if (dir == "vertical" || dir == "column" || dir == "col") {
                max_cross = std::max(max_cross, it.bbox.width());
            }
        }

        double grid_cell_w = 0.0;
        double grid_cell_h = 0.0;
        if (dir == "grid") {
            for (const auto& it : items) {
                grid_cell_w = std::max(grid_cell_w, it.bbox.width());
                grid_cell_h = std::max(grid_cell_h, it.bbox.height());
            }
        }

        BBox children_bounds;
        std::vector<ShapeNode> laid_out;
        for (size_t i = 0; i < items.size(); ++i) {
            ShapeNode ch = items[i];
            double w = ch.bbox.width();
            double h = ch.bbox.height();
            double target_x = cur_x;
            double target_y = cur_y;

            if (dir == "horizontal" || dir == "row") {
                if (align == "center" || align == "middle") target_y += (max_cross - h) * 0.5;
                else if (align == "end" || align == "bottom") target_y += (max_cross - h);
                cur_x += w + gap_x;
            } else if (dir == "vertical" || dir == "column" || dir == "col") {
                if (align == "center" || align == "middle") target_x += (max_cross - w) * 0.5;
                else if (align == "end" || align == "right") target_x += (max_cross - w);
                cur_y += h + gap_y;
            } else {
                int r = static_cast<int>(i) / cols;
                int c = static_cast<int>(i) % cols;
                target_x = start_x + padding + c * (grid_cell_w + gap_x);
                target_y = start_y + padding + r * (grid_cell_h + gap_y);
            }

            double dx = target_x - ch.bbox.min_x;
            double dy = target_y - ch.bbox.min_y;
            ch.transform = Transform2D::translate(dx, dy).multiply(ch.transform);
            compute_shape_bbox_and_register(ch, &scene_);
            children_bounds.merge(ch.bbox);
            laid_out.push_back(std::move(ch));
        }

        if (children_bounds.valid && (padding > 0.0 || container_style.fill.kind != PaintKind::None || container_style.stroke.kind != PaintKind::None)) {
            ShapeNode bg_rect;
            bg_rect.kind = ShapeKind::Rect;
            bg_rect.x = start_x;
            bg_rect.y = start_y;
            bg_rect.width = (children_bounds.max_x - start_x) + padding;
            bg_rect.height = (children_bounds.max_y - start_y) + padding;
            bg_rect.corner_radius = container_radius;
            for (int k = 0; k < 4; ++k) bg_rect.corner_radii[k] = container_radius;
            bg_rect.style = container_style;
            compute_shape_bbox_and_register(bg_rect, nullptr);
            group.children.push_back(std::move(bg_rect));
        }

        for (auto& ch : laid_out) {
            group.children.push_back(std::move(ch));
        }

        return group;
    }

    BlendMode parse_blend_mode() {
        std::string bm = consume().text;
        if (bm == "add" || bm == "additive" || bm == "plus") return BlendMode::Add;
        if (bm == "screen") return BlendMode::Screen;
        if (bm == "multiply") return BlendMode::Multiply;
        if (bm == "overlay") return BlendMode::Overlay;
        return BlendMode::Normal;
    }

    ArrowMode parse_arrow_mode() {
        std::string am = consume().text;
        if (am == "end" || am == "forward" || am == "true") return ArrowMode::End;
        if (am == "start" || am == "backward") return ArrowMode::Start;
        if (am == "both" || am == "bidirectional") return ArrowMode::Both;
        return ArrowMode::None;
    }

    void parse_gradient_decl() {
        std::string kind_str = consume().text;
        std::string id;
        if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
            id = consume().text;
        } else {
            throw ParseError("Expected gradient identifier after '" + kind_str + "'", peek().line, peek().col);
        }

        PaintServer ps;
        ps.id = id;
        if (kind_str == "linear_gradient") ps.kind = PaintKind::LinearGradient;
        else if (kind_str == "radial_gradient") ps.kind = PaintKind::RadialGradient;
        else if (kind_str == "conic_gradient") ps.kind = PaintKind::ConicGradient;
        else if (kind_str == "mesh_gradient") ps.kind = PaintKind::MeshGradient;
        else if (kind_str == "noise_gradient") ps.kind = PaintKind::NoiseGradient;
        else ps.kind = PaintKind::DiffusionGradient;

        bool has_from_to = false;
        bool has_angle = false;

        expect(TokKind::LBrace, "Expected '{' to open gradient block");
        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;

            std::string item = consume().text;
            if (item == "stop" && peek().kind == TokKind::LParen) {
                consume(); // '('
                double offset = parse_expr();
                expect(TokKind::Comma, "Expected ',' after stop offset");
                Color col = parse_color();
                expect(TokKind::RParen, "Expected ')' after stop(offset, color)");
                ps.stops.push_back({clamp01(offset), col});
            } else if ((item == "point" || item == "patch") && peek().kind == TokKind::LParen) {
                consume(); // '('
                double u = parse_expr();
                expect(TokKind::Comma, "Expected ',' after point u");
                double v = parse_expr();
                expect(TokKind::Comma, "Expected ',' after point v");
                Color col = parse_color();
                double weight = 1.0;
                if (peek().kind == TokKind::Comma) {
                    consume();
                    if (peek().kind == TokKind::Ident && peek().text == "weight" && peek(1).kind == TokKind::Colon) {
                        consume();
                        consume();
                    }
                    weight = parse_expr();
                }
                expect(TokKind::RParen, "Expected ')' after point(...)");
                if (ps.kind == PaintKind::MeshGradient) {
                    ps.grid_points.push_back({u, v, col, weight});
                } else {
                    ps.diffusion_points.push_back({u, v, col, weight});
                }
            } else {
                expect(TokKind::Colon, "Expected ':' after gradient property '" + item + "'");
                if (item == "from" || item == "start") { ps.start = parse_vec2(); has_from_to = true; }
                else if (item == "to" || item == "end") { ps.end = parse_vec2(); has_from_to = true; }
                else if (item == "center") ps.center = parse_vec2();
                else if (item == "radius" || item == "r") ps.radius = parse_expr();
                else if (item == "angle") { ps.angle_deg = parse_expr(); has_angle = true; }
                else if (item == "rows") ps.rows = static_cast<int>(std::round(parse_expr()));
                else if (item == "cols") ps.cols = static_cast<int>(std::round(parse_expr()));
                else if (item == "falloff") ps.falloff_exponent = parse_expr();
                else if (item == "scale" || item == "frequency") ps.noise_scale = parse_expr();
                else if (item == "octaves") ps.noise_octaves = static_cast<int>(std::round(parse_expr()));
                else if (item == "persistence" || item == "gain") ps.noise_persistence = parse_expr();
                else if (item == "lacunarity") ps.noise_lacunarity = parse_expr();
                else if (item == "seed") ps.noise_seed = parse_expr();
                else if (item == "grain" || item == "noise_grain") ps.grain = clamp01(parse_expr());
                else if (item == "space") {
                    std::string sp = consume().text;
                    ps.user_space = (sp == "user" || sp == "canvas" || sp == "world");
                } else {
                    throw ParseError("Unknown gradient property '" + item + "'", peek().line, peek().col);
                }
            }
            skip_separators();
        }
        expect(TokKind::RBrace, "Expected '}' after gradient block");

        if (ps.kind == PaintKind::LinearGradient && has_angle && !has_from_to) {
            double rad = ps.angle_deg * PI / 180.0;
            double dx = 0.5 * std::cos(rad);
            double dy = 0.5 * std::sin(rad);
            ps.start = {0.5 - dx, 0.5 - dy};
            ps.end = {0.5 + dx, 0.5 + dy};
        }

        std::sort(ps.stops.begin(), ps.stops.end(),
                  [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; });
        ps.build_luts();
        scene_.paint_servers[id] = ps;
    }

    ShapeNode parse_shape_decl() {
        if (peek().kind == TokKind::Ident && peek().text == "use") {
            return parse_use_decl();
        }
        if (peek().kind == TokKind::Ident && peek().text == "repeat") {
            return parse_repeat_decl();
        }
        if (peek().kind == TokKind::Ident && (peek().text == "stack" || peek().text == "grid")) {
            return parse_stack_decl();
        }

        const Token& kw_tok = consume();
        std::string kw = kw_tok.text;

        ShapeNode node;
        bool is_star = (kw == "star");
        bool is_ngon = (kw == "ngon" || kw == "regular_polygon" || kw == "hexagon");
        bool is_ring = (kw == "ring" || kw == "annulus");
        bool is_arc  = (kw == "arc" || kw == "sector");

        if (kw == "rect") node.kind = ShapeKind::Rect;
        else if (kw == "circle") node.kind = ShapeKind::Circle;
        else if (kw == "ellipse") node.kind = ShapeKind::Ellipse;
        else if (kw == "line") node.kind = ShapeKind::Line;
        else if (kw == "polygon" || is_star || is_ngon) { node.kind = ShapeKind::Polygon; node.closed = true; }
        else if (kw == "polyline" || is_arc) { node.kind = ShapeKind::Polyline; node.closed = false; }
        else if (kw == "spline") { node.kind = ShapeKind::Spline; node.closed = false; }
        else if (kw == "path" || kw == "contour") node.kind = ShapeKind::Path;
        else if (kw == "boolean" || is_ring) node.kind = ShapeKind::Boolean;
        else if (kw == "text") node.kind = ShapeKind::Text;
        else if (kw == "group" || kw == "layer") node.kind = ShapeKind::Group;
        else {
            throw ParseError("Unknown element keyword '" + kw + "'", kw_tok.line, kw_tok.col);
        }

        if (peek().kind == TokKind::String || peek().kind == TokKind::Ident) {
            node.id = consume().text;
        }

        expect(TokKind::LBrace, "Expected '{' to open '" + kw + "' block");

        bool has_pos_center = false;
        Vec2 pos_center = {0, 0};

        // Parametric shape variables
        int star_rays = 5;
        int ngon_sides = (kw == "hexagon") ? 6 : 6;
        double inner_radius = 0.0;
        double start_angle_deg = 0.0;
        double end_angle_deg = 180.0;

        while (!at_eof() && peek().kind != TokKind::RBrace) {
            skip_separators();
            if (peek().kind == TokKind::RBrace) break;

            if (peek().kind == TokKind::Ident && peek().text == "let") {
                parse_let_decl();
                continue;
            }

            if ((node.kind == ShapeKind::Group || node.kind == ShapeKind::Boolean) &&
                !is_ring && peek().kind == TokKind::Ident && is_shape_keyword(peek().text)) {
                ShapeNode child = parse_shape_decl();
                compute_shape_bbox_and_register(child, &scene_);
                node.children.push_back(std::move(child));
                skip_separators();
                continue;
            }

            if (node.kind == ShapeKind::Path && peek().kind == TokKind::Ident &&
                (peek(1).kind == TokKind::LParen || peek().text == "close")) {
                parse_path_command(node);
                skip_separators();
                continue;
            }

            if (peek().kind != TokKind::Ident) {
                throw ParseError("Expected property name inside '" + kw + "', got '" + peek().text + "'",
                                 peek().line, peek().col);
            }
            std::string prop = consume().text;

            // Forgiving syntax: support both `shadow(dx, dy, blur, col)` AND `shadow: (dx, dy, blur, col)`
            if (prop == "shadow") {
                parse_shadow_args(node.style.shadow);
                skip_separators();
                continue;
            }
            if (prop == "glow") {
                parse_glow_args(node.style.glow);
                skip_separators();
                continue;
            }

            expect(TokKind::Colon, "Expected ':' after property '" + prop + "'");

            if (prop == "x") {
                double x = parse_expr();
                node.x = x;
                if (node.kind == ShapeKind::Text) node.text_pos.x = x;
            } else if (prop == "y") {
                double y = parse_expr();
                node.y = y;
                if (node.kind == ShapeKind::Text) node.text_pos.y = y;
            } else if (prop == "pos" || prop == "top_left") {
                Vec2 p = parse_vec2();
                node.x = p.x;
                node.y = p.y;
                node.text_pos = p;
            } else if (prop == "at") {
                Vec2 p = parse_vec2();
                node.text_pos = p;
                node.center = p;
                has_pos_center = true;
                pos_center = p;
            } else if (prop == "center") {
                Vec2 p = parse_vec2();
                node.center = p;
                if (node.kind == ShapeKind::Text) node.text_pos = p;
                has_pos_center = true;
                pos_center = p;
            } else if (prop == "width" || prop == "w") node.width = parse_expr();
            else if (prop == "height" || prop == "h") node.height = parse_expr();
            else if (prop == "size") {
                if (node.kind == ShapeKind::Text) {
                    node.font_size = parse_expr();
                } else if (peek().kind == TokKind::LParen) {
                    Vec2 sz = parse_vec2();
                    node.width = sz.x;
                    node.height = sz.y;
                } else {
                    double s = parse_expr();
                    node.font_size = s;
                    node.width = s;
                    node.height = s;
                }
            } else if (prop == "radius" || prop == "r" || prop == "outer_radius") {
                if (peek().kind == TokKind::LBracket) {
                    consume(); // '['
                    for (int i = 0; i < 4; ++i) {
                        if (i > 0 && peek().kind == TokKind::Comma) consume();
                        node.corner_radii[i] = parse_expr();
                    }
                    expect(TokKind::RBracket, "Expected ']' after 4-corner radius array");
                    node.corner_radius = node.corner_radii[0];
                } else {
                    double r = parse_expr();
                    node.radius = r;
                    node.corner_radius = r;
                    for (int i = 0; i < 4; ++i) node.corner_radii[i] = r;
                }
            } else if (prop == "inner_radius" || prop == "hole_radius") {
                inner_radius = parse_expr();
            } else if (prop == "thickness") {
                double th = parse_expr();
                if (node.radius > th) inner_radius = node.radius - th;
            } else if (prop == "rays" || prop == "arms") {
                star_rays = std::clamp(static_cast<int>(std::round(parse_expr())), 3, 128);
            } else if (prop == "sides") {
                ngon_sides = std::clamp(static_cast<int>(std::round(parse_expr())), 3, 256);
            } else if (prop == "start_angle") {
                start_angle_deg = parse_expr();
            } else if (prop == "end_angle") {
                end_angle_deg = parse_expr();
            } else if (prop == "rx") node.rx = parse_expr();
            else if (prop == "ry") node.ry = parse_expr();
            else if (prop == "radii") {
                Vec2 rr = parse_vec2();
                node.rx = rr.x;
                node.ry = rr.y;
            } else if (prop == "from" || prop == "start") node.from = parse_vec2();
            else if (prop == "to" || prop == "end") node.to = parse_vec2();
            else if (prop == "points") {
                if (is_star && peek().kind != TokKind::LBracket) {
                    star_rays = std::clamp(static_cast<int>(std::round(parse_expr())), 3, 128);
                } else {
                    expect(TokKind::LBracket, "Expected '[' to start points list");
                    while (!at_eof() && peek().kind != TokKind::RBracket) {
                        skip_separators();
                        if (peek().kind == TokKind::RBrace || peek().kind == TokKind::RBracket) break;
                        node.points.push_back(parse_vec2());
                        skip_separators();
                    }
                    expect(TokKind::RBracket, "Expected ']' after points list");
                }
            } else if (prop == "tension") node.tension = parse_expr();
            else if (prop == "closed") {
                if (peek().kind == TokKind::Ident) {
                    std::string b = consume().text;
                    node.closed = (b == "true" || b == "yes");
                } else {
                    node.closed = (parse_expr() != 0.0);
                }
            } else if (prop == "op") {
                std::string op_name = consume().text;
                if (op_name == "union") node.bool_op = BooleanOp::Union;
                else if (op_name == "subtract" || op_name == "difference") node.bool_op = BooleanOp::Subtract;
                else if (op_name == "intersect") node.bool_op = BooleanOp::Intersect;
                else if (op_name == "smooth_union") {
                    node.bool_op = BooleanOp::SmoothUnion;
                    if (peek().kind == TokKind::LParen) {
                        consume();
                        node.smoothness = parse_expr();
                        expect(TokKind::RParen, "Expected ')' after smooth_union(k)");
                    } else if (node.smoothness <= 0.0) {
                        node.smoothness = 16.0;
                    }
                } else {
                    throw ParseError("Unknown boolean op '" + op_name + "'", peek().line, peek().col);
                }
            } else if (prop == "smoothness" || prop == "k") {
                node.smoothness = parse_expr();
            } else if (prop == "content" || prop == "text") {
                node.text_content = consume().text;
            } else if (prop == "font") {
                if (peek().kind == TokKind::Ref) {
                    node.font_id = consume().text;
                } else {
                    std::string value = parse_font_string_value();
                    if (scene_.fonts.count(value)) node.font_id = value;
                    else node.font_family = value;
                }
            } else if (prop == "font_family" || prop == "font_name") {
                node.font_family = parse_font_string_value();
            } else if (prop == "font_src" || prop == "font_file") {
                node.font_src = parse_font_string_value();
            } else if (prop == "font_fallback") {
                node.font_fallback = parse_font_string_value();
            } else if (prop == "font_style" || prop == "font_slant") {
                node.font_style_explicit = true;
                if (peek().kind == TokKind::Ident) {
                    node.font_style = font_style_from_name(consume().text);
                } else {
                    int s = static_cast<int>(std::round(parse_expr()));
                    node.font_style = (s == 1) ? FontStyle::Italic : (s == 2 ? FontStyle::Oblique : FontStyle::Normal);
                }
            } else if (prop == "font_stretch" || prop == "font_width") {
                node.font_stretch_explicit = true;
                if (peek().kind == TokKind::Ident) {
                    node.font_stretch = font_stretch_from_name(consume().text);
                } else {
                    node.font_stretch = parse_expr();
                }
            } else if (prop == "font_features" || prop == "features") {
                node.font_features = parse_font_string_value();
            } else if (prop == "font_language" || prop == "lang") {
                node.font_language = parse_font_string_value();
            } else if (prop == "font_weight" || (node.kind == ShapeKind::Text && prop == "weight")) {
                node.font_weight_explicit = true;
                if (peek().kind == TokKind::Ident) {
                    node.font_weight = font_weight_from_name(consume().text);
                } else {
                    node.font_weight = parse_expr();
                }
            } else if (prop == "line_height" || prop == "leading") {
                node.line_height = parse_expr();
            } else if (prop == "max_width" || prop == "wrap_width") {
                node.max_width = parse_expr();
            } else if (prop == "wrap") {
                if (peek().kind == TokKind::Ident) {
                    std::string b = consume().text;
                    node.wrap = (b == "true" || b == "yes" || b == "on");
                } else {
                    node.wrap = (parse_expr() != 0.0);
                }
            } else if (prop == "baseline" || prop == "baseline_anchor") {
                if (peek().kind == TokKind::Ident) {
                    std::string b = consume().text;
                    node.baseline_anchor = (b == "baseline" || b == "true" || b == "yes" || b == "on");
                } else {
                    node.baseline_anchor = (parse_expr() != 0.0);
                }
            } else if (prop == "word_spacing") {
                node.word_spacing = parse_expr();
            } else if (prop == "align") {
                std::string al = consume().text;
                if (al == "center" || al == "middle") node.text_align = TextAlign::Center;
                else if (al == "right" || al == "end") node.text_align = TextAlign::Right;
                else node.text_align = TextAlign::Left;
            } else if (prop == "letter_spacing" || prop == "tracking") {
                node.letter_spacing = parse_expr();
            } else if (prop == "fill") {
                node.style.fill = parse_paint_value();
                if (node.style.fill.grain > 0.0 && node.style.grain <= 0.0) {
                    node.style.grain = node.style.fill.grain;
                }
            } else if (prop == "stroke") {
                node.style.stroke = parse_paint_value();
                if (node.style.stroke_width <= 0.0 && node.style.stroke.kind != PaintKind::None) {
                    node.style.stroke_width = 1.5;
                }
            } else if (prop == "stroke_width" || prop == "weight") {
                node.style.stroke_width = parse_expr();
            } else if (prop == "stroke_align") {
                std::string sa = consume().text;
                if (sa == "inner" || sa == "inside") node.style.stroke_align = StrokeAlign::Inner;
                else if (sa == "outer" || sa == "outside") node.style.stroke_align = StrokeAlign::Outer;
                else node.style.stroke_align = StrokeAlign::Center;
            } else if (prop == "cap" || prop == "line_cap") {
                std::string cp = consume().text;
                if (cp == "butt" || cp == "flat") node.style.line_cap = LineCap::Butt;
                else if (cp == "square") node.style.line_cap = LineCap::Square;
                else node.style.line_cap = LineCap::Round;
            } else if (prop == "join" || prop == "line_join") {
                std::string jn = consume().text;
                if (jn == "miter") node.style.line_join = LineJoin::Miter;
                else if (jn == "bevel") node.style.line_join = LineJoin::Bevel;
                else node.style.line_join = LineJoin::Round;
            } else if (prop == "dash") {
                expect(TokKind::LBracket, "Expected '[' for dash array");
                while (!at_eof() && peek().kind != TokKind::RBracket) {
                    skip_separators();
                    if (peek().kind == TokKind::RBracket) break;
                    node.style.dash_array.push_back(parse_expr());
                    skip_separators();
                }
                expect(TokKind::RBracket, "Expected ']' after dash array");
            } else if (prop == "dash_offset") {
                node.style.dash_offset = parse_expr();
            } else if (prop == "grain" || prop == "noise_grain") {
                node.style.grain = clamp01(parse_expr());
            } else if (prop == "arrow" || prop == "marker") {
                node.style.arrow_mode = parse_arrow_mode();
            } else if (prop == "arrow_size") {
                node.style.arrow_size = parse_expr();
            } else if (prop == "opacity" || prop == "alpha") {
                node.style.opacity = clamp01(parse_expr());
            } else if (prop == "blend" || prop == "blend_mode") {
                node.style.blend_mode = parse_blend_mode();
            } else if (prop == "clip" || prop == "clip_to") {
                node.style.clip_id = consume().text;
            } else if (prop == "matrix" || prop == "transform") {
                expect(TokKind::LParen, "Expected '(' after matrix");
                Transform2D m;
                m.a = parse_expr();
                expect(TokKind::Comma, "Expected ',' between matrix components");
                m.b = parse_expr();
                expect(TokKind::Comma, "Expected ',' between matrix components");
                m.c = parse_expr();
                expect(TokKind::Comma, "Expected ',' between matrix components");
                m.d = parse_expr();
                expect(TokKind::Comma, "Expected ',' between matrix components");
                m.tx = parse_expr();
                expect(TokKind::Comma, "Expected ',' between matrix components");
                m.ty = parse_expr();
                expect(TokKind::RParen, "Expected ')' after matrix");
                node.transform = node.transform.multiply(m);
            } else if (prop == "rotate") {
                double deg = parse_expr();
                Vec2 piv = node.center;
                if (node.kind == ShapeKind::Rect) {
                    piv = {has_pos_center ? pos_center.x : (node.x + node.width * 0.5),
                           has_pos_center ? pos_center.y : (node.y + node.height * 0.5)};
                }
                node.transform = node.transform.multiply(Transform2D::rotate_deg(deg, piv));
            } else if (prop == "translate") {
                Vec2 tr = parse_vec2();
                node.transform = node.transform.multiply(Transform2D::translate(tr.x, tr.y));
            } else if (prop == "scale") {
                if (peek().kind == TokKind::LParen) {
                    Vec2 sc = parse_vec2();
                    node.transform = node.transform.multiply(Transform2D::scale(sc.x, sc.y));
                } else {
                    double s = parse_expr();
                    node.transform = node.transform.multiply(Transform2D::scale(s, s));
                }
            } else {
                throw ParseError("Unknown property '" + prop + "' on '" + kw + "'", peek().line, peek().col);
            }

            skip_separators();
        }

        expect(TokKind::RBrace, "Expected '}' to close '" + kw + "' block");

        if (node.kind == ShapeKind::Rect && has_pos_center) {
            node.x = pos_center.x - node.width * 0.5;
            node.y = pos_center.y - node.height * 0.5;
        }

        // Synthesize parametric geometry for `star`, `ngon`, `ring`, `arc`
        if (is_star) {
            double r_out = node.radius;
            double r_in = (inner_radius > 0.0) ? inner_radius : (r_out * 0.42);
            node.points.clear();
            int total_v = star_rays * 2;
            for (int i = 0; i < total_v; ++i) {
                double a_rad = (-90.0 + (360.0 * i) / total_v) * PI / 180.0;
                double r = (i % 2 == 0) ? r_out : r_in;
                node.points.push_back({node.center.x + r * std::cos(a_rad), node.center.y + r * std::sin(a_rad)});
            }
        } else if (is_ngon) {
            double r_out = node.radius;
            node.points.clear();
            for (int i = 0; i < ngon_sides; ++i) {
                double a_rad = (-90.0 + (360.0 * i) / ngon_sides) * PI / 180.0;
                node.points.push_back({node.center.x + r_out * std::cos(a_rad), node.center.y + r_out * std::sin(a_rad)});
            }
        } else if (is_ring) {
            node.bool_op = BooleanOp::Subtract;
            ShapeNode c_out;
            c_out.kind = ShapeKind::Circle;
            c_out.center = node.center;
            c_out.radius = node.radius;
            ShapeNode c_in;
            c_in.kind = ShapeKind::Circle;
            c_in.center = node.center;
            c_in.radius = (inner_radius > 0.0) ? inner_radius : (node.radius * 0.65);
            node.children = {c_out, c_in};
        } else if (is_arc) {
            node.points.clear();
            double span = end_angle_deg - start_angle_deg;
            int steps = std::clamp(static_cast<int>(std::ceil(std::abs(span) * 0.22)), 12, 36);
            for (int i = 0; i <= steps; ++i) {
                double a_rad = (start_angle_deg + span * (static_cast<double>(i) / steps)) * PI / 180.0;
                node.points.push_back({node.center.x + node.radius * std::cos(a_rad), node.center.y + node.radius * std::sin(a_rad)});
            }
            if (kw == "sector" || node.closed || inner_radius > 0.0) {
                node.kind = ShapeKind::Polygon;
                node.closed = true;
                if (inner_radius > 0.0) {
                    for (int i = steps; i >= 0; --i) {
                        double a_rad = (start_angle_deg + span * (static_cast<double>(i) / steps)) * PI / 180.0;
                        node.points.push_back({node.center.x + inner_radius * std::cos(a_rad), node.center.y + inner_radius * std::sin(a_rad)});
                    }
                } else {
                    node.points.push_back(node.center);
                }
            }
        }

        if (node.kind == ShapeKind::Text && !node.text_content.empty()) {
            Result lr = layout_text_node(node, scene_, scene_.base_dir);
            if (!lr.ok) {
                throw ParseError(lr.error, kw_tok.line, kw_tok.col);
            }
        }

        return node;
    }

    void parse_path_command(ShapeNode& node) {
        std::string cmd = consume().text;
        if (cmd == "close") {
            if (peek().kind == TokKind::LParen) {
                consume();
                expect(TokKind::RParen, "Expected ')' after close(");
            }
            PathSegment seg;
            seg.verb = PathVerb::Close;
            node.path_segments.push_back(seg);
            node.closed = true;
            return;
        }

        expect(TokKind::LParen, "Expected '(' after path command '" + cmd + "'");
        if (cmd == "move") {
            Vec2 p = parse_vec2();
            expect(TokKind::RParen, "Expected ')' after move(x, y)");
            PathSegment seg;
            seg.verb = PathVerb::MoveTo;
            seg.p1 = p;
            node.path_segments.push_back(seg);
        } else if (cmd == "line") {
            Vec2 p = parse_vec2();
            expect(TokKind::RParen, "Expected ')' after line(x, y)");
            PathSegment seg;
            seg.verb = PathVerb::LineTo;
            seg.p1 = p;
            node.path_segments.push_back(seg);
        } else if (cmd == "quad") {
            Vec2 c = parse_vec2();
            expect(TokKind::Comma, "Expected ',' between quad control point and end point");
            Vec2 p = parse_vec2();
            expect(TokKind::RParen, "Expected ')' after quad(...)");
            PathSegment seg;
            seg.verb = PathVerb::QuadTo;
            seg.p1 = c;
            seg.p2 = p;
            node.path_segments.push_back(seg);
        } else if (cmd == "cubic") {
            Vec2 c1 = parse_vec2();
            expect(TokKind::Comma, "Expected ',' after cubic control point 1");
            Vec2 c2 = parse_vec2();
            expect(TokKind::Comma, "Expected ',' after cubic control point 2");
            Vec2 p = parse_vec2();
            expect(TokKind::RParen, "Expected ')' after cubic(...)");
            PathSegment seg;
            seg.verb = PathVerb::CubicTo;
            seg.p1 = c1;
            seg.p2 = c2;
            seg.p3 = p;
            node.path_segments.push_back(seg);
        } else if (cmd == "bulge") {
            Vec2 p = parse_vec2();
            expect(TokKind::Comma, "Expected ',' before bulge curvature factor");
            if (peek().kind == TokKind::Ident && (peek().text == "curvature" || peek().text == "b") &&
                peek(1).kind == TokKind::Colon) {
                consume();
                consume();
            }
            double b = parse_expr();
            expect(TokKind::RParen, "Expected ')' after bulge(x, y, curvature)");
            PathSegment seg;
            seg.verb = PathVerb::BulgeTo;
            seg.p1 = p;
            seg.bulge = b;
            node.path_segments.push_back(seg);
        } else if (cmd == "arc_to") {
            Vec2 p = parse_vec2();
            expect(TokKind::Comma, "Expected ',' before arc_to radius");
            if (peek().kind == TokKind::Ident && (peek().text == "r" || peek().text == "radius") &&
                peek(1).kind == TokKind::Colon) {
                consume();
                consume();
            }
            double r = parse_expr();
            bool cw = true;
            bool large = false;
            if (peek().kind == TokKind::Comma) {
                consume();
                if (peek().kind == TokKind::Ident && (peek().text == "cw" || peek().text == "clockwise") &&
                    peek(1).kind == TokKind::Colon) {
                    consume();
                    consume();
                }
                cw = (parse_expr() != 0.0);
            }
            if (peek().kind == TokKind::Comma) {
                consume();
                if (peek().kind == TokKind::Ident && peek().text == "large" && peek(1).kind == TokKind::Colon) {
                    consume();
                    consume();
                }
                large = (parse_expr() != 0.0);
            }
            expect(TokKind::RParen, "Expected ')' after arc_to(...)");
            PathSegment seg;
            seg.verb = PathVerb::ArcTo;
            seg.p1 = p;
            seg.radius = r;
            seg.clockwise = cw;
            seg.large_arc = large;
            node.path_segments.push_back(seg);
        } else {
            throw ParseError("Unknown path command '" + cmd + "'", peek().line, peek().col);
        }
    }

    const std::vector<Token>& tokens_;
    Scene& scene_;
    size_t idx_ = 0;
};

Result parse_and_evaluate(const std::string& source, Scene& out_scene, double time_sec,
                          const std::string& base_dir) {
    out_scene = Scene{};
    out_scene.eval_time = time_sec;
    out_scene.base_dir = base_dir;
    try {
        Lexer lex(source);
        auto tokens = lex.tokenize();
        Parser parser(tokens, out_scene);
        parser.parse_document();
        return {true, "", 0, 0};
    } catch (const ParseError& e) {
        std::ostringstream oss;
        oss << "Line " << e.line << ":" << e.col << " - " << e.what();
        return {false, oss.str(), e.line, e.col};
    } catch (const std::exception& e) {
        return {false, e.what(), 0, 0};
    }
}

} // namespace mesh
