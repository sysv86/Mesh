// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "../include/mesh.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace mesh {

// ============================================================================
// 1. Analytical Signed Distance Fields (SDF), Arc-Length & Arrowheads
// ============================================================================

static double sd_segment_ex(const Vec2& p, const Vec2& a, const Vec2& b, double* out_h = nullptr) {
    Vec2 pa = p - a;
    Vec2 ba = b - a;
    double denom = ba.length_sq();
    double h = denom > 1e-12 ? std::clamp(pa.dot(ba) / denom, 0.0, 1.0) : 0.0;
    if (out_h) *out_h = h;
    return (pa - ba * h).length();
}

static double sd_triangle(const Vec2& p, const Vec2& p0, const Vec2& p1, const Vec2& p2) {
    Vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    Vec2 v0 = p - p0,  v1 = p - p1,  v2 = p - p2;
    Vec2 pq0 = v0 - e0 * std::clamp(v0.dot(e0) / std::max(1e-12, e0.length_sq()), 0.0, 1.0);
    Vec2 pq1 = v1 - e1 * std::clamp(v1.dot(e1) / std::max(1e-12, e1.length_sq()), 0.0, 1.0);
    Vec2 pq2 = v2 - e2 * std::clamp(v2.dot(e2) / std::max(1e-12, e2.length_sq()), 0.0, 1.0);
    double s = (e0.x * e2.y - e0.y * e2.x) >= 0.0 ? 1.0 : -1.0;
    double d_sq = std::min({pq0.length_sq(), pq1.length_sq(), pq2.length_sq()});
    double c0 = s * (v0.x * e0.y - v0.y * e0.x);
    double c1 = s * (v1.x * e1.y - v1.y * e1.x);
    double c2 = s * (v2.x * e2.y - v2.y * e2.x);
    double min_c = std::min({c0, c1, c2});
    return -std::sqrt(d_sq) * (min_c >= 0.0 ? 1.0 : -1.0);
}

static double sd_arrowhead(const Vec2& p, const Vec2& tip, const Vec2& dir_norm, double size) {
    Vec2 perp = {-dir_norm.y, dir_norm.x};
    Vec2 base = tip - dir_norm * size;
    Vec2 left = base + perp * (size * 0.55);
    Vec2 right = base - perp * (size * 0.55);
    return sd_triangle(p, tip, left, right);
}

static inline double sd_rounded_box(const Vec2& p, const Vec2& half_extents, const double r[4]) {
    double cr = (p.x > 0.0) ? ((p.y > 0.0) ? r[2] : r[1])
                            : ((p.y > 0.0) ? r[3] : r[0]);
    cr = std::min(cr, std::min(half_extents.x, half_extents.y));
    double qx = std::abs(p.x) - half_extents.x + cr;
    double qy = std::abs(p.y) - half_extents.y + cr;
    if (qx <= 0.0 && qy <= 0.0) return std::max(qx, qy) - cr;
    if (qx <= 0.0) return qy - cr;
    if (qy <= 0.0) return qx - cr;
    return std::sqrt(qx * qx + qy * qy) - cr;
}

static double sd_ellipse_approx(const Vec2& p, double rx, double ry) {
    rx = std::max(1e-6, rx);
    ry = std::max(1e-6, ry);
    if (std::abs(rx - ry) < 1e-6) {
        return p.length() - rx;
    }
    Vec2 q = {p.x / rx, p.y / ry};
    double r = q.length();
    if (r < 1e-9) return -std::min(rx, ry);
    Vec2 grad = {p.x / (rx * rx), p.y / (ry * ry)};
    double g_len = grad.length();
    return (r - 1.0) * (r / std::max(1e-9, g_len));
}

static double sd_polygon(const std::vector<Vec2>& pts, const Vec2& p, double* out_arc = nullptr) {
    if (pts.size() < 2) return 1e9;
    double d_sq = 1e18;
    double s = 1.0;
    size_t n = pts.size();
    if (!out_arc) {
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            Vec2 e = pts[i] - pts[j];
            Vec2 w = p - pts[j];
            double denom = e.x * e.x + e.y * e.y;
            double h = denom > 1e-12 ? std::clamp((w.x * e.x + w.y * e.y) / denom, 0.0, 1.0) : 0.0;
            double bx = w.x - e.x * h;
            double by = w.y - e.y * h;
            double cur_d2 = bx * bx + by * by;
            if (cur_d2 < d_sq) d_sq = cur_d2;

            bool c1 = p.y >= pts[j].y;
            bool c2 = p.y < pts[i].y;
            bool c3 = (e.x * w.y > e.y * w.x);
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
        }
        return s * std::sqrt(d_sq);
    }
    double best_arc = 0.0;
    double acc_arc = 0.0;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        Vec2 e = pts[i] - pts[j];
        Vec2 w = p - pts[j];
        double seg_len = e.length();
        double denom = seg_len * seg_len;
        double h = denom > 1e-12 ? std::clamp(w.dot(e) / denom, 0.0, 1.0) : 0.0;
        Vec2 b = w - e * h;
        double cur_d2 = b.length_sq();
        if (cur_d2 < d_sq) {
            d_sq = cur_d2;
            best_arc = acc_arc + h * seg_len;
        }
        acc_arc += seg_len;

        bool c1 = p.y >= pts[j].y;
        bool c2 = p.y < pts[i].y;
        bool c3 = (e.x * w.y > e.y * w.x);
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    *out_arc = best_arc;
    return s * std::sqrt(d_sq);
}

static inline double sd_segment_sq(const Vec2& p, const Vec2& a, const Vec2& b, double* out_h = nullptr) {
    double pax = p.x - a.x, pay = p.y - a.y;
    double bax = b.x - a.x, bay = b.y - a.y;
    double denom = bax * bax + bay * bay;
    double h = denom > 1e-12 ? std::clamp((pax * bax + pay * bay) / denom, 0.0, 1.0) : 0.0;
    if (out_h) *out_h = h;
    double dx = pax - bax * h;
    double dy = pay - bay * h;
    return dx * dx + dy * dy;
}

static double sd_subpaths(const std::vector<FlattenedSubpath>& subpaths, const Vec2& p, bool treat_closed, double* out_arc = nullptr) {
    if (subpaths.empty()) return 1e9;
    double min_d2 = treat_closed ? 1e18 : 2304.0;
    double best_arc = 0.0;
    int winding = 0;

    for (const auto& sp : subpaths) {
        if (sp.points.size() < 2) continue;
        bool is_closed = sp.closed || treat_closed;
        if (is_closed && min_d2 == 2304.0) min_d2 = 1e18;
        size_t n = sp.points.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            const Vec2& a = sp.points[i];
            const Vec2& b = sp.points[i + 1];
            if (!is_closed && min_d2 < 1e16) {
                double dx_min = (p.x < a.x && p.x < b.x) ? (std::min(a.x, b.x) - p.x)
                              : ((p.x > a.x && p.x > b.x) ? (p.x - std::max(a.x, b.x)) : 0.0);
                if (dx_min * dx_min >= min_d2) continue;
                double dy_min = (p.y < a.y && p.y < b.y) ? (std::min(a.y, b.y) - p.y)
                              : ((p.y > a.y && p.y > b.y) ? (p.y - std::max(a.y, b.y)) : 0.0);
                if (dx_min * dx_min + dy_min * dy_min >= min_d2) continue;
            }
            double h = 0.0;
            double d2 = sd_segment_sq(p, a, b, out_arc ? &h : nullptr);
            if (d2 < min_d2) {
                min_d2 = d2;
                if (out_arc && i < sp.cumulative_lengths.size()) {
                    double seg_l = (i + 1 < sp.cumulative_lengths.size())
                                       ? (sp.cumulative_lengths[i + 1] - sp.cumulative_lengths[i])
                                       : (b - a).length();
                    best_arc = sp.cumulative_lengths[i] + h * seg_l;
                }
            }

            if (is_closed) {
                if (a.y <= p.y) {
                    if (b.y > p.y && ((b - a).cross(p - a) > 0.0)) winding++;
                } else {
                    if (b.y <= p.y && ((b - a).cross(p - a) < 0.0)) winding--;
                }
            }
        }
        if (is_closed && (sp.points.back() - sp.points.front()).length_sq() > 1e-6) {
            const Vec2& a = sp.points.back();
            const Vec2& b = sp.points.front();
            double h = 0.0;
            double d2 = sd_segment_sq(p, a, b, out_arc ? &h : nullptr);
            if (d2 < min_d2) {
                min_d2 = d2;
                if (out_arc && !sp.cumulative_lengths.empty()) {
                    best_arc = sp.cumulative_lengths.back() + h * (b - a).length();
                }
            }
            if (a.y <= p.y) {
                if (b.y > p.y && ((b - a).cross(p - a) > 0.0)) winding++;
            } else {
                if (b.y <= p.y && ((b - a).cross(p - a) < 0.0)) winding--;
            }
        }
    }

    if (out_arc) *out_arc = best_arc;
    double min_dist = std::sqrt(min_d2);
    bool inside = (winding != 0);
    return inside ? -min_dist : min_dist;
}

double evaluate_shape_sdf(const ShapeNode& node, const Vec2& p_world, double* out_arc_len) {
    if (node.kind == ShapeKind::Boolean) {
        if (node.children.empty()) return 1e9;
        double d = evaluate_shape_sdf(node.children[0], p_world, out_arc_len);
        for (size_t i = 1; i < node.children.size(); ++i) {
            double d_b = evaluate_shape_sdf(node.children[i], p_world, nullptr);
            switch (node.bool_op) {
                case BooleanOp::Union:
                    d = std::min(d, d_b);
                    break;
                case BooleanOp::Subtract:
                    d = std::max(d, -d_b);
                    break;
                case BooleanOp::Intersect:
                    d = std::max(d, d_b);
                    break;
                case BooleanOp::SmoothUnion: {
                    double k = std::max(1e-4, node.smoothness);
                    double h = std::clamp(0.5 + 0.5 * (d_b - d) / k, 0.0, 1.0);
                    d = (d_b + (d - d_b) * h) - k * h * (1.0 - h);
                    break;
                }
            }
        }
        return d;
    }

    if (node.kind == ShapeKind::Group) {
        double d = 1e9;
        for (const auto& ch : node.children) {
            d = std::min(d, evaluate_shape_sdf(ch, p_world, out_arc_len));
        }
        return d;
    }

    Vec2 p = (node.scale_factor == 1.0 && node.inv_transform.tx == 0.0 && node.inv_transform.ty == 0.0 && node.inv_transform.a == 1.0 && node.inv_transform.b == 0.0) ? p_world : node.inv_transform.apply(p_world);

    double local_d = 1e9;
    switch (node.kind) {
        case ShapeKind::Rect: {
            Vec2 c = {node.x + node.width * 0.5, node.y + node.height * 0.5};
            Vec2 half = {node.width * 0.5, node.height * 0.5};
            local_d = sd_rounded_box(p - c, half, node.corner_radii);
            if (out_arc_len) {
                double ang = std::atan2(p.y - c.y, p.x - c.x);
                if (ang < 0) ang += 2.0 * PI;
                *out_arc_len = (ang / (2.0 * PI)) * (2.0 * (node.width + node.height));
            }
            break;
        }
        case ShapeKind::Circle: {
            Vec2 diff = p - node.center;
            local_d = diff.length() - node.radius;
            if (out_arc_len) {
                double ang = std::atan2(diff.y, diff.x);
                if (ang < 0) ang += 2.0 * PI;
                *out_arc_len = ang * node.radius;
            }
            break;
        }
        case ShapeKind::Ellipse: {
            Vec2 diff = p - node.center;
            local_d = sd_ellipse_approx(diff, node.rx, node.ry);
            if (out_arc_len) {
                double ang = std::atan2(diff.y, diff.x);
                if (ang < 0) ang += 2.0 * PI;
                *out_arc_len = ang * 0.5 * (node.rx + node.ry);
            }
            break;
        }
        case ShapeKind::Line: {
            double h = 0.0;
            local_d = sd_segment_ex(p, node.from, node.to, &h);
            if (out_arc_len) *out_arc_len = h * (node.to - node.from).length();
            break;
        }
        case ShapeKind::Polygon: {
            local_d = sd_polygon(node.points, p, out_arc_len);
            break;
        }
        case ShapeKind::Polyline: {
            double min_d2 = 1e18;
            double best_arc = 0.0;
            for (size_t i = 0; i + 1 < node.points.size(); ++i) {
                double h = 0.0;
                double d2 = sd_segment_sq(p, node.points[i], node.points[i + 1], out_arc_len ? &h : nullptr);
                if (d2 < min_d2) {
                    min_d2 = d2;
                    if (out_arc_len && i < node.point_lengths.size()) {
                        double seg_l = (i + 1 < node.point_lengths.size())
                                           ? (node.point_lengths[i + 1] - node.point_lengths[i])
                                           : (node.points[i + 1] - node.points[i]).length();
                        best_arc = node.point_lengths[i] + h * seg_l;
                    }
                }
            }
            if (out_arc_len) *out_arc_len = best_arc;
            local_d = std::sqrt(min_d2);
            break;
        }
        case ShapeKind::Spline:
        case ShapeKind::Path: {
            bool has_fill = (node.style.fill.kind != PaintKind::None) || node.closed;
            local_d = sd_subpaths(node.subpaths, p, has_fill, out_arc_len);
            break;
        }
        case ShapeKind::Text: {
            if (!node.text_subpaths.empty()) {
                local_d = sd_subpaths(node.text_subpaths, p, true, out_arc_len);
            }
            break;
        }
        default:
            break;
    }
    return local_d * node.scale_factor;
}

static double evaluate_node_arrowheads_sdf(const ShapeNode& node, const Vec2& p_world) {
    if (node.style.arrow_mode == ArrowMode::None) return 1e9;
    Vec2 p = node.inv_transform.apply(p_world);
    double sz = std::max(4.0, node.style.arrow_size);

    Vec2 start_pt, start_dir, end_pt, end_dir;
    bool valid = false;

    if (node.kind == ShapeKind::Line) {
        Vec2 d = node.to - node.from;
        if (d.length_sq() > 1e-6) {
            start_pt = node.from;
            start_dir = (node.from - node.to).normalized();
            end_pt = node.to;
            end_dir = d.normalized();
            valid = true;
        }
    } else if (node.kind == ShapeKind::Polyline && node.points.size() >= 2) {
        start_pt = node.points.front();
        start_dir = (node.points.front() - node.points[1]).normalized();
        end_pt = node.points.back();
        end_dir = (node.points.back() - node.points[node.points.size() - 2]).normalized();
        valid = true;
    } else if ((node.kind == ShapeKind::Spline || node.kind == ShapeKind::Path) && !node.subpaths.empty()) {
        const auto& sp0 = node.subpaths.front();
        const auto& spN = node.subpaths.back();
        if (sp0.points.size() >= 2 && spN.points.size() >= 2) {
            start_pt = sp0.points.front();
            start_dir = (sp0.points.front() - sp0.points[1]).normalized();
            end_pt = spN.points.back();
            end_dir = (spN.points.back() - spN.points[spN.points.size() - 2]).normalized();
            valid = true;
        }
    }

    if (!valid) return 1e9;
    double d_arrow = 1e9;
    if (node.style.arrow_mode == ArrowMode::End || node.style.arrow_mode == ArrowMode::Both) {
        d_arrow = std::min(d_arrow, sd_arrowhead(p, end_pt + end_dir * (sz * 0.25), end_dir, sz));
    }
    if (node.style.arrow_mode == ArrowMode::Start || node.style.arrow_mode == ArrowMode::Both) {
        d_arrow = std::min(d_arrow, sd_arrowhead(p, start_pt + start_dir * (sz * 0.25), start_dir, sz));
    }
    return d_arrow * node.scale_factor;
}

static double evaluate_dash_mask(const Style& style, double arc_len, double aa_width) {
    if (style.dash_array.empty() || style.dash_period <= 1e-6) return 1.0;
    double s = std::fmod(arc_len - style.dash_offset, style.dash_period);
    if (s < 0.0) s += style.dash_period;

    double acc = 0.0;
    for (size_t i = 0; i < style.dash_array.size(); ++i) {
        double seg = style.dash_array[i];
        if (s >= acc && s <= acc + seg) {
            bool is_on = (i % 2 == 0);
            double dist_to_edge = std::min(s - acc, (acc + seg) - s);
            if (is_on) {
                return std::clamp(0.5 + dist_to_edge / std::max(1e-4, aa_width), 0.0, 1.0);
            } else {
                return std::clamp(0.5 - dist_to_edge / std::max(1e-4, aa_width), 0.0, 1.0);
            }
        }
        acc += seg;
    }
    return 1.0;
}

// ============================================================================
// 2. Built-in Zero-Dependency Scalable Vector Font Engine (Full ASCII 32..126)
// ============================================================================

struct GlyphStroke {
    double x0, y0, x1, y1;
};

static std::vector<GlyphStroke> get_glyph_strokes(char ch) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    switch (c) {
        case 'A': return {{0.05,0.95, 0.50,0.05}, {0.50,0.05, 0.95,0.95}, {0.20,0.62, 0.80,0.62}};
        case 'B': return {{0.10,0.05, 0.10,0.95}, {0.10,0.05, 0.72,0.05}, {0.72,0.05, 0.88,0.20}, {0.88,0.20, 0.88,0.38}, {0.88,0.38, 0.72,0.50}, {0.10,0.50, 0.75,0.50}, {0.75,0.50, 0.90,0.64}, {0.90,0.64, 0.90,0.82}, {0.90,0.82, 0.72,0.95}, {0.72,0.95, 0.10,0.95}};
        case 'C': return {{0.88,0.22, 0.70,0.05}, {0.70,0.05, 0.30,0.05}, {0.30,0.05, 0.10,0.25}, {0.10,0.25, 0.10,0.75}, {0.10,0.75, 0.30,0.95}, {0.30,0.95, 0.70,0.95}, {0.70,0.95, 0.88,0.78}};
        case 'D': return {{0.10,0.05, 0.10,0.95}, {0.10,0.05, 0.65,0.05}, {0.65,0.05, 0.90,0.30}, {0.90,0.30, 0.90,0.70}, {0.90,0.70, 0.65,0.95}, {0.65,0.95, 0.10,0.95}};
        case 'E': return {{0.12,0.05, 0.12,0.95}, {0.12,0.05, 0.88,0.05}, {0.12,0.50, 0.75,0.50}, {0.12,0.95, 0.88,0.95}};
        case 'F': return {{0.12,0.05, 0.12,0.95}, {0.12,0.05, 0.88,0.05}, {0.12,0.50, 0.75,0.50}};
        case 'G': return {{0.88,0.22, 0.70,0.05}, {0.70,0.05, 0.28,0.05}, {0.28,0.05, 0.10,0.25}, {0.10,0.25, 0.10,0.75}, {0.10,0.75, 0.28,0.95}, {0.28,0.95, 0.75,0.95}, {0.75,0.95, 0.90,0.78}, {0.90,0.78, 0.90,0.52}, {0.90,0.52, 0.52,0.52}};
        case 'H': return {{0.12,0.05, 0.12,0.95}, {0.88,0.05, 0.88,0.95}, {0.12,0.50, 0.88,0.50}};
        case 'I': return {{0.25,0.05, 0.75,0.05}, {0.50,0.05, 0.50,0.95}, {0.25,0.95, 0.75,0.95}};
        case 'J': return {{0.25,0.05, 0.85,0.05}, {0.65,0.05, 0.65,0.75}, {0.65,0.75, 0.45,0.95}, {0.45,0.95, 0.22,0.95}, {0.22,0.95, 0.10,0.78}};
        case 'K': return {{0.12,0.05, 0.12,0.95}, {0.88,0.05, 0.12,0.55}, {0.35,0.40, 0.88,0.95}};
        case 'L': return {{0.12,0.05, 0.12,0.95}, {0.12,0.95, 0.88,0.95}};
        case 'M': return {{0.08,0.95, 0.08,0.05}, {0.08,0.05, 0.50,0.60}, {0.50,0.60, 0.92,0.05}, {0.92,0.05, 0.92,0.95}};
        case 'N': return {{0.12,0.95, 0.12,0.05}, {0.12,0.05, 0.88,0.95}, {0.88,0.95, 0.88,0.05}};
        case 'O': return {{0.30,0.05, 0.70,0.05}, {0.70,0.05, 0.90,0.25}, {0.90,0.25, 0.90,0.75}, {0.90,0.75, 0.70,0.95}, {0.70,0.95, 0.30,0.95}, {0.30,0.95, 0.10,0.75}, {0.10,0.75, 0.10,0.25}, {0.10,0.25, 0.30,0.05}};
        case 'P': return {{0.12,0.05, 0.12,0.95}, {0.12,0.05, 0.72,0.05}, {0.72,0.05, 0.90,0.20}, {0.90,0.20, 0.90,0.40}, {0.90,0.40, 0.72,0.55}, {0.72,0.55, 0.12,0.55}};
        case 'Q': return {{0.30,0.05, 0.70,0.05}, {0.70,0.05, 0.90,0.25}, {0.90,0.25, 0.90,0.75}, {0.90,0.75, 0.70,0.95}, {0.70,0.95, 0.30,0.95}, {0.30,0.95, 0.10,0.75}, {0.10,0.75, 0.10,0.25}, {0.10,0.25, 0.30,0.05}, {0.55,0.65, 0.92,0.98}};
        case 'R': return {{0.12,0.05, 0.12,0.95}, {0.12,0.05, 0.72,0.05}, {0.72,0.05, 0.90,0.20}, {0.90,0.20, 0.90,0.40}, {0.90,0.40, 0.72,0.52}, {0.72,0.52, 0.12,0.52}, {0.52,0.52, 0.90,0.95}};
        case 'S': return {{0.88,0.20, 0.70,0.05}, {0.70,0.05, 0.28,0.05}, {0.28,0.05, 0.10,0.22}, {0.10,0.22, 0.10,0.38}, {0.10,0.38, 0.28,0.48}, {0.28,0.48, 0.72,0.52}, {0.72,0.52, 0.90,0.65}, {0.90,0.65, 0.90,0.80}, {0.90,0.80, 0.70,0.95}, {0.70,0.95, 0.28,0.95}, {0.28,0.95, 0.10,0.80}};
        case 'T': return {{0.08,0.05, 0.92,0.05}, {0.50,0.05, 0.50,0.95}};
        case 'U': return {{0.12,0.05, 0.12,0.75}, {0.12,0.75, 0.30,0.95}, {0.30,0.95, 0.70,0.95}, {0.70,0.95, 0.88,0.75}, {0.88,0.75, 0.88,0.05}};
        case 'V': return {{0.08,0.05, 0.50,0.95}, {0.50,0.95, 0.92,0.05}};
        case 'W': return {{0.05,0.05, 0.26,0.95}, {0.26,0.95, 0.50,0.42}, {0.50,0.42, 0.74,0.95}, {0.74,0.95, 0.95,0.05}};
        case 'X': return {{0.10,0.05, 0.90,0.95}, {0.90,0.05, 0.10,0.95}};
        case 'Y': return {{0.10,0.05, 0.50,0.52}, {0.90,0.05, 0.50,0.52}, {0.50,0.52, 0.50,0.95}};
        case 'Z': return {{0.10,0.05, 0.90,0.05}, {0.90,0.05, 0.10,0.95}, {0.10,0.95, 0.90,0.95}};
        case '0': return {{0.30,0.05, 0.70,0.05}, {0.70,0.05, 0.88,0.25}, {0.88,0.25, 0.88,0.75}, {0.88,0.75, 0.70,0.95}, {0.70,0.95, 0.30,0.95}, {0.30,0.95, 0.12,0.75}, {0.12,0.75, 0.12,0.25}, {0.12,0.25, 0.30,0.05}, {0.22,0.80, 0.78,0.20}};
        case '1': return {{0.28,0.22, 0.52,0.05}, {0.52,0.05, 0.52,0.95}, {0.25,0.95, 0.78,0.95}};
        case '2': return {{0.12,0.25, 0.30,0.05}, {0.30,0.05, 0.72,0.05}, {0.72,0.05, 0.88,0.22}, {0.88,0.22, 0.88,0.42}, {0.88,0.42, 0.12,0.95}, {0.12,0.95, 0.90,0.95}};
        case '3': return {{0.15,0.18, 0.32,0.05}, {0.32,0.05, 0.72,0.05}, {0.72,0.05, 0.88,0.22}, {0.88,0.22, 0.72,0.48}, {0.42,0.48, 0.72,0.48}, {0.72,0.48, 0.90,0.68}, {0.90,0.68, 0.72,0.95}, {0.72,0.95, 0.30,0.95}, {0.30,0.95, 0.12,0.80}};
        case '4': return {{0.72,0.95, 0.72,0.05}, {0.72,0.05, 0.10,0.65}, {0.10,0.65, 0.92,0.65}};
        case '5': return {{0.85,0.05, 0.15,0.05}, {0.15,0.05, 0.15,0.48}, {0.15,0.48, 0.70,0.48}, {0.70,0.48, 0.88,0.65}, {0.88,0.65, 0.88,0.80}, {0.88,0.80, 0.70,0.95}, {0.70,0.95, 0.25,0.95}, {0.25,0.95, 0.12,0.82}};
        case '6': return {{0.82,0.15, 0.65,0.05}, {0.65,0.05, 0.30,0.05}, {0.30,0.05, 0.12,0.28}, {0.12,0.28, 0.12,0.75}, {0.12,0.75, 0.30,0.95}, {0.30,0.95, 0.72,0.95}, {0.72,0.95, 0.88,0.75}, {0.88,0.75, 0.88,0.60}, {0.88,0.60, 0.72,0.48}, {0.72,0.48, 0.12,0.48}};
        case '7': return {{0.10,0.05, 0.90,0.05}, {0.90,0.05, 0.35,0.95}};
        case '8': return {{0.30,0.05, 0.70,0.05}, {0.70,0.05, 0.88,0.22}, {0.88,0.22, 0.70,0.48}, {0.70,0.48, 0.30,0.48}, {0.30,0.48, 0.12,0.22}, {0.12,0.22, 0.30,0.05}, {0.70,0.48, 0.90,0.72}, {0.90,0.72, 0.70,0.95}, {0.70,0.95, 0.30,0.95}, {0.30,0.95, 0.10,0.72}, {0.10,0.72, 0.30,0.48}};
        case '9': return {{0.88,0.52, 0.28,0.52}, {0.28,0.52, 0.12,0.35}, {0.12,0.35, 0.12,0.22}, {0.12,0.22, 0.30,0.05}, {0.30,0.05, 0.70,0.05}, {0.70,0.05, 0.88,0.25}, {0.88,0.25, 0.88,0.75}, {0.88,0.75, 0.68,0.95}, {0.68,0.95, 0.28,0.95}};
        case '-': return {{0.18,0.50, 0.82,0.50}};
        case '+': return {{0.18,0.50, 0.82,0.50}, {0.50,0.20, 0.50,0.80}};
        case '=': return {{0.18,0.36, 0.82,0.36}, {0.18,0.64, 0.82,0.64}};
        case '.': return {{0.46,0.86, 0.54,0.95}};
        case ',': return {{0.52,0.82, 0.40,1.02}};
        case ':': return {{0.46,0.30, 0.54,0.38}, {0.46,0.75, 0.54,0.84}};
        case ';': return {{0.46,0.30, 0.54,0.38}, {0.52,0.75, 0.40,0.98}};
        case '/': return {{0.82,0.08, 0.18,0.92}};
        case '|': return {{0.50,0.04, 0.50,0.96}};
        case '_': return {{0.05,0.95, 0.95,0.95}};
        case '(': return {{0.68,0.05, 0.35,0.35}, {0.35,0.35, 0.35,0.65}, {0.35,0.65, 0.68,0.95}};
        case ')': return {{0.32,0.05, 0.65,0.35}, {0.65,0.35, 0.65,0.65}, {0.65,0.65, 0.32,0.95}};
        case '[': return {{0.68,0.05, 0.30,0.05}, {0.30,0.05, 0.30,0.95}, {0.30,0.95, 0.68,0.95}};
        case ']': return {{0.32,0.05, 0.70,0.05}, {0.70,0.05, 0.70,0.95}, {0.70,0.95, 0.32,0.95}};
        case '>': return {{0.22,0.18, 0.78,0.50}, {0.78,0.50, 0.22,0.82}};
        case '<': return {{0.78,0.18, 0.22,0.50}, {0.22,0.50, 0.78,0.82}};
        case '%': return {{0.85,0.10, 0.15,0.90}, {0.22,0.15, 0.32,0.25}, {0.68,0.75, 0.78,0.85}};
        case '$': return {{0.50,0.02, 0.50,0.98}, {0.82,0.20, 0.65,0.10}, {0.65,0.10, 0.32,0.10}, {0.32,0.10, 0.16,0.26}, {0.16,0.26, 0.32,0.46}, {0.32,0.46, 0.68,0.54}, {0.68,0.54, 0.84,0.72}, {0.84,0.72, 0.68,0.90}, {0.68,0.90, 0.32,0.90}, {0.32,0.90, 0.16,0.78}};
        case '&': return {{0.85,0.95, 0.22,0.32}, {0.22,0.32, 0.22,0.15}, {0.22,0.15, 0.45,0.05}, {0.45,0.05, 0.68,0.15}, {0.68,0.15, 0.68,0.32}, {0.68,0.32, 0.15,0.65}, {0.15,0.65, 0.15,0.85}, {0.15,0.85, 0.42,0.95}, {0.42,0.95, 0.85,0.62}};
        case '~': return {{0.12,0.58, 0.34,0.38}, {0.34,0.38, 0.66,0.62}, {0.66,0.62, 0.88,0.42}};
        case '^': return {{0.20,0.42, 0.50,0.08}, {0.50,0.08, 0.80,0.42}};
        case '{': return {{0.68,0.05, 0.45,0.12}, {0.45,0.12, 0.45,0.42}, {0.45,0.42, 0.25,0.50}, {0.25,0.50, 0.45,0.58}, {0.45,0.58, 0.45,0.88}, {0.45,0.88, 0.68,0.95}};
        case '}': return {{0.32,0.05, 0.55,0.12}, {0.55,0.12, 0.55,0.42}, {0.55,0.42, 0.75,0.50}, {0.75,0.50, 0.55,0.58}, {0.55,0.58, 0.55,0.88}, {0.55,0.88, 0.32,0.95}};
        case '\\': return {{0.18,0.08, 0.82,0.92}};
        case '"': return {{0.35,0.05, 0.35,0.32}, {0.65,0.05, 0.65,0.32}};
        case '\'': return {{0.50,0.05, 0.50,0.32}};
        case '#': return {{0.32,0.10, 0.28,0.90}, {0.72,0.10, 0.68,0.90}, {0.12,0.36, 0.88,0.36}, {0.12,0.64, 0.88,0.64}};
        case '@': return {{0.75,0.65, 0.55,0.72}, {0.55,0.72, 0.35,0.60}, {0.35,0.60, 0.35,0.40}, {0.35,0.40, 0.55,0.32}, {0.55,0.32, 0.72,0.45}, {0.72,0.45, 0.72,0.68}, {0.88,0.50, 0.75,0.12}, {0.75,0.12, 0.25,0.12}, {0.25,0.12, 0.10,0.50}, {0.10,0.50, 0.25,0.88}, {0.25,0.88, 0.78,0.88}};
        case '*': return {{0.50,0.18, 0.50,0.82}, {0.18,0.32, 0.82,0.68}, {0.82,0.32, 0.18,0.68}};
        case '!': return {{0.50,0.05, 0.50,0.68}, {0.50,0.86, 0.50,0.95}};
        case '?': return {{0.20,0.22, 0.38,0.05}, {0.38,0.05, 0.72,0.05}, {0.72,0.05, 0.85,0.25}, {0.85,0.25, 0.50,0.55}, {0.50,0.55, 0.50,0.70}, {0.50,0.86, 0.50,0.95}};
        default: return {};
    }
}

// ============================================================================
// 3. Coarse Tile-Binning Multithreaded Rasterizer
// ============================================================================

struct TileRect {
    int x0, y0, x1, y1;
};

struct TileBin {
    TileRect rect;
    std::vector<const ShapeNode*> active_nodes;
};

static inline double pixel_grain_dither(int px, int py) {
    uint32_t h = static_cast<uint32_t>(px) * 1973u + static_cast<uint32_t>(py) * 9277u + 89173u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h & 0xFFFFu) / 32768.0) - 1.0;
}

static inline double blend_channel(double dst, double src, BlendMode mode) {
    switch (mode) {
        case BlendMode::Add:      return clamp01(dst + src);
        case BlendMode::Screen:   return 1.0 - (1.0 - dst) * (1.0 - src);
        case BlendMode::Multiply: return dst * src;
        case BlendMode::Overlay:  return (dst < 0.5) ? (2.0 * dst * src) : (1.0 - 2.0 * (1.0 - dst) * (1.0 - src));
        default:                  return src;
    }
}

static inline void blend_pixel_mode(uint8_t* dst, const Color& src_col, double coverage, BlendMode mode = BlendMode::Normal) {
    double a = clamp01(src_col.a * coverage);
    if (a <= 0.0019) return;
    if (mode == BlendMode::Normal) {
        uint32_t sr = static_cast<uint32_t>(clamp01(src_col.r) * 255.0 + 0.5);
        uint32_t sg = static_cast<uint32_t>(clamp01(src_col.g) * 255.0 + 0.5);
        uint32_t sb = static_cast<uint32_t>(clamp01(src_col.b) * 255.0 + 0.5);
        if (a >= 0.998) {
            dst[0] = static_cast<uint8_t>(sr);
            dst[1] = static_cast<uint8_t>(sg);
            dst[2] = static_cast<uint8_t>(sb);
            dst[3] = 255;
            return;
        }
        if (dst[3] == 255) {
            uint32_t ia = static_cast<uint32_t>(a * 256.0 + 0.5);
            uint32_t inv_a = 256u - ia;
            dst[0] = static_cast<uint8_t>((sr * ia + dst[0] * inv_a) >> 8);
            dst[1] = static_cast<uint8_t>((sg * ia + dst[1] * inv_a) >> 8);
            dst[2] = static_cast<uint8_t>((sb * ia + dst[2] * inv_a) >> 8);
            return;
        }
    }

    double dst_r = dst[0] * (1.0 / 255.0);
    double dst_g = dst[1] * (1.0 / 255.0);
    double dst_b = dst[2] * (1.0 / 255.0);
    double dst_a = dst[3] * (1.0 / 255.0);

    double eff_r = (mode == BlendMode::Normal) ? src_col.r : blend_channel(dst_r, src_col.r, mode);
    double eff_g = (mode == BlendMode::Normal) ? src_col.g : blend_channel(dst_g, src_col.g, mode);
    double eff_b = (mode == BlendMode::Normal) ? src_col.b : blend_channel(dst_b, src_col.b, mode);

    double out_a = a + dst_a * (1.0 - a);
    if (out_a > 1e-6) {
        double inv_out_a = 1.0 / out_a;
        double out_r = (eff_r * a + dst_r * dst_a * (1.0 - a)) * inv_out_a;
        double out_g = (eff_g * a + dst_g * dst_a * (1.0 - a)) * inv_out_a;
        double out_b = (eff_b * a + dst_b * dst_a * (1.0 - a)) * inv_out_a;
        dst[0] = static_cast<uint8_t>(clamp01(out_r) * 255.0 + 0.5);
        dst[1] = static_cast<uint8_t>(clamp01(out_g) * 255.0 + 0.5);
        dst[2] = static_cast<uint8_t>(clamp01(out_b) * 255.0 + 0.5);
        dst[3] = static_cast<uint8_t>(clamp01(out_a) * 255.0 + 0.5);
    }
}

static bool is_open_curve(const ShapeNode& node) {
    if (node.kind == ShapeKind::Line || node.kind == ShapeKind::Polyline) return true;
    if ((node.kind == ShapeKind::Spline || node.kind == ShapeKind::Path) &&
        !node.closed && node.style.fill.kind == PaintKind::None) {
        return true;
    }
    return false;
}

static void flatten_leaf_nodes(const ShapeNode& node, std::vector<const ShapeNode*>& out_leaves) {
    if (node.kind == ShapeKind::Group) {
        for (const auto& ch : node.children) {
            flatten_leaf_nodes(ch, out_leaves);
        }
    } else {
        out_leaves.push_back(&node);
    }
}

static void render_text_node_tile(const Scene& scene, const ShapeNode& node, RasterImage& img,
                                  double scale_x, double scale_y, const TileRect& tile) {
    if (node.text_content.empty()) return;
    double scale = 0.5 * (scale_x + scale_y);
    double h = node.font_size * scale;
    double w = h * 0.62;
    double adv = w + node.letter_spacing * scale;
    if (adv <= 1e-6) adv = 1.0;
    double total_w = static_cast<double>(node.text_content.size()) * adv - node.letter_spacing * scale;

    Vec2 anchor = node.transform.apply(node.text_pos);
    anchor.x *= scale_x;
    anchor.y *= scale_y;

    double start_x = anchor.x;
    if (node.text_align == TextAlign::Center) start_x -= total_w * 0.5;
    else if (node.text_align == TextAlign::Right) start_x -= total_w;
    double start_y = anchor.y - h * 0.5;

    double weight_factor = std::clamp(node.font_weight / 600.0, 0.55, 1.55);
    double stroke_r = std::max(0.82, h * 0.068 * weight_factor);

    struct WorldSeg { Vec2 a, b; };
    size_t n_chars = node.text_content.size();
    std::vector<std::vector<WorldSeg>> char_segs(n_chars);
    BBox text_bb;
    bool any_seg = false;

    for (size_t i = 0; i < n_chars; ++i) {
        char ch = node.text_content[i];
        double cx = start_x + i * adv;
        auto strokes = get_glyph_strokes(ch);
        for (const auto& st : strokes) {
            Vec2 p0 = {cx + st.x0 * w, start_y + st.y0 * h};
            Vec2 p1 = {cx + st.x1 * w, start_y + st.y1 * h};
            char_segs[i].push_back({p0, p1});
            text_bb.include(p0);
            text_bb.include(p1);
            any_seg = true;
        }
    }

    if (!any_seg) return;
    double pad = stroke_r + 2.5;
    if (node.style.glow.enabled) pad = std::max(pad, stroke_r + node.style.glow.radius * scale + 2.0);
    if (node.style.shadow.enabled) {
        pad = std::max(pad, stroke_r + (std::max(std::abs(node.style.shadow.dx), std::abs(node.style.shadow.dy)) + node.style.shadow.blur) * scale + 2.0);
    }
    text_bb.expand(pad);

    int x0 = std::max(tile.x0, static_cast<int>(std::floor(text_bb.min_x)));
    int y0 = std::max(tile.y0, static_cast<int>(std::floor(text_bb.min_y)));
    int x1 = std::min(tile.x1, static_cast<int>(std::ceil(text_bb.max_x)));
    int y1 = std::min(tile.y1, static_cast<int>(std::ceil(text_bb.max_y)));
    if (x0 > x1 || y0 > y1) return;

    const ShapeNode* clip_node = nullptr;
    if (!node.style.clip_id.empty()) {
        auto it = scene.named_nodes.find(node.style.clip_id);
        if (it != scene.named_nodes.end()) clip_node = &it->second;
    }

    int reach_chars = static_cast<int>(std::ceil(pad / adv)) + 1;

    auto eval_char_window = [&](const Vec2& q) -> double {
        int center_i = static_cast<int>(std::floor((q.x - start_x) / adv));
        int c0 = std::max(0, center_i - reach_chars);
        int c1 = std::min(static_cast<int>(n_chars) - 1, center_i + reach_chars);
        double min_d2 = 1e18;
        for (int ci = c0; ci <= c1; ++ci) {
            for (const auto& sg : char_segs[ci]) {
                Vec2 pa = q - sg.a;
                Vec2 ba = sg.b - sg.a;
                double denom = ba.x * ba.x + ba.y * ba.y;
                double h_t = denom > 1e-12 ? std::clamp((pa.x * ba.x + pa.y * ba.y) / denom, 0.0, 1.0) : 0.0;
                double dx = pa.x - ba.x * h_t;
                double dy = pa.y - ba.y * h_t;
                double d2 = dx * dx + dy * dy;
                if (d2 < min_d2) min_d2 = d2;
            }
        }
        return std::sqrt(min_d2);
    };

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            Vec2 p = {px + 0.5, py + 0.5};
            Vec2 p_world = {p.x / scale_x, p.y / scale_y};
            double clip_cov = 1.0;
            if (clip_node) {
                double dc = evaluate_shape_sdf(*clip_node, p_world);
                clip_cov = std::clamp(0.5 - dc * scale, 0.0, 1.0);
                if (clip_cov <= 0.0) continue;
            }
            uint8_t* dst = &img.rgba[(py * img.width + px) * 4];

            if (node.style.shadow.enabled) {
                Vec2 p_sh = {p.x - node.style.shadow.dx * scale_x, p.y - node.style.shadow.dy * scale_y};
                double d_sh = eval_char_window(p_sh) - stroke_r;
                double blur_px = std::max(1.0, node.style.shadow.blur * scale);
                if (d_sh < blur_px) {
                    double t = std::clamp(1.0 - (std::max(0.0, d_sh) / blur_px), 0.0, 1.0);
                    double sh_cov = t * t * (3.0 - 2.0 * t) * clip_cov;
                    if (sh_cov > 0.0) {
                        Color sc = node.style.shadow.color;
                        sc.a *= node.style.opacity;
                        blend_pixel_mode(dst, sc, sh_cov, node.style.blend_mode);
                    }
                }
            }

            double d_edge = eval_char_window(p) - stroke_r;

            if (node.style.glow.enabled) {
                double gr_px = std::max(1.0, node.style.glow.radius * scale);
                if (d_edge > -1.0 && d_edge < gr_px) {
                    double t = std::clamp(1.0 - (std::max(0.0, d_edge) / gr_px), 0.0, 1.0);
                    double g_cov = t * t * clip_cov;
                    if (g_cov > 0.0) {
                        Color gc = node.style.glow.color;
                        gc.a *= node.style.opacity;
                        blend_pixel_mode(dst, gc, g_cov, node.style.blend_mode);
                    }
                }
            }

            double cov = std::clamp(0.5 - d_edge, 0.0, 1.0) * clip_cov;
            if (cov > 0.0) {
                Color col = (node.style.fill.kind != PaintKind::None)
                                ? node.style.fill.sample(p_world, node.bbox)
                                : ((node.style.stroke.kind != PaintKind::None)
                                       ? node.style.stroke.sample(p_world, node.bbox)
                                       : Color::from_srgb(1, 1, 1, 1));
                col.a *= node.style.opacity;
                blend_pixel_mode(dst, col, cov, node.style.blend_mode);
            }
        }
    }
}

static void render_shape_node_tile(const Scene& scene, const ShapeNode& node, RasterImage& img,
                                   double scale_x, double scale_y, int ssaa, const TileRect& tile) {
    if (node.kind == ShapeKind::Text && node.text_subpaths.empty()) {
        // No real font outlines resolved: keep the built-in vector font as a
        // deterministic fallback for zero-dependency / WASM builds.
        render_text_node_tile(scene, node, img, scale_x, scale_y, tile);
        return;
    }

    double scale = 0.5 * (scale_x + scale_y);
    double inv_sx = 1.0 / scale_x;
    double inv_sy = 1.0 / scale_y;

    const BBox& bounds = node.render_bounds;
    if (!bounds.valid) return;

    int x0 = std::max(tile.x0, static_cast<int>(std::floor(bounds.min_x * scale_x)));
    int y0 = std::max(tile.y0, static_cast<int>(std::floor(bounds.min_y * scale_y)));
    int x1 = std::min(tile.x1, static_cast<int>(std::ceil(bounds.max_x * scale_x)));
    int y1 = std::min(tile.y1, static_cast<int>(std::ceil(bounds.max_y * scale_y)));
    if (x0 > x1 || y0 > y1) return;

    bool open_curve = is_open_curve(node);
    bool has_fill = !open_curve && (node.style.fill.kind != PaintKind::None);
    bool has_stroke = (node.style.stroke.kind != PaintKind::None) && (node.style.stroke_width > 0.0);
    bool has_dashes = has_stroke && !node.style.dash_array.empty() && node.style.dash_period > 1e-6;
    bool has_arrows = (node.style.arrow_mode != ArrowMode::None);
    double aa_width = 1.0 / std::max(0.5, scale);

    double max_outer_reach = aa_width * 2.0 + node.style.stroke_width;
    if (node.style.glow.enabled) max_outer_reach = std::max(max_outer_reach, node.style.glow.radius + aa_width * 2.0);

    const ShapeNode* clip_node = nullptr;
    if (!node.style.clip_id.empty()) {
        auto it = scene.named_nodes.find(node.style.clip_id);
        if (it != scene.named_nodes.end()) clip_node = &it->second;
    }

    static const double rgss_x[4] = {0.125, 0.625, 0.375, 0.875};
    static const double rgss_y[4] = {0.375, 0.125, 0.875, 0.625};
    bool use_rgss = (ssaa >= 4);

    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            Vec2 p_center = {(px + 0.5) * inv_sx, (py + 0.5) * inv_sy};
            uint8_t* dst = &img.rgba[(py * img.width + px) * 4];

            double clip_cov = 1.0;
            if (clip_node) {
                double d_clip = evaluate_shape_sdf(*clip_node, p_center);
                clip_cov = std::clamp(0.5 - d_clip / aa_width, 0.0, 1.0);
                if (clip_cov <= 0.0) continue;
            }

            double arc_len = 0.0;
            double d = evaluate_shape_sdf(node, p_center, has_dashes ? &arc_len : nullptr);

            if (node.style.shadow.enabled) {
                double sh_reach = std::abs(node.style.shadow.dx) + std::abs(node.style.shadow.dy) + node.style.shadow.blur;
                if ((!has_fill || d > -aa_width * 1.5) && d < sh_reach + aa_width * 2.0) {
                    Vec2 p_sh = {p_center.x - node.style.shadow.dx, p_center.y - node.style.shadow.dy};
                    double d_sh = evaluate_shape_sdf(node, p_sh);
                    double blur = std::max(1.0, node.style.shadow.blur);
                    if (d_sh < blur) {
                        double t = std::clamp(1.0 - (d_sh / blur), 0.0, 1.0);
                        double sh_cov = t * t * (3.0 - 2.0 * t) * clip_cov;
                        if (sh_cov > 0.0) {
                            Color sc = node.style.shadow.color;
                            sc.a *= node.style.opacity;
                            blend_pixel_mode(dst, sc, sh_cov, node.style.blend_mode);
                        }
                    }
                }
            }

            if (!has_arrows && d > max_outer_reach) {
                continue;
            }

            if (node.style.glow.enabled && d > -aa_width && d < node.style.glow.radius) {
                double gr = std::max(1.0, node.style.glow.radius);
                double t = std::clamp(1.0 - (std::max(0.0, d) / gr), 0.0, 1.0);
                double g_cov = t * t * clip_cov;
                if (g_cov > 0.0) {
                    Color gc = node.style.glow.color;
                    gc.a *= node.style.opacity;
                    blend_pixel_mode(dst, gc, g_cov, node.style.blend_mode);
                }
            }

            if (has_fill && d < aa_width * 1.5) {
                double fill_cov = 0.0;
                if (use_rgss && std::abs(d) <= aa_width * 1.5) {
                    for (int s = 0; s < 4; ++s) {
                        Vec2 ps = {(px + rgss_x[s]) * inv_sx, (py + rgss_y[s]) * inv_sy};
                        double ds = evaluate_shape_sdf(node, ps);
                        fill_cov += std::clamp(0.5 - ds / (aa_width * 0.75), 0.0, 1.0);
                    }
                    fill_cov *= 0.25;
                } else {
                    fill_cov = std::clamp(0.5 - d / aa_width, 0.0, 1.0);
                }
                fill_cov *= clip_cov;
                if (fill_cov > 0.0) {
                    Color fc = node.style.fill.sample(p_center, node.bbox);
                    if (node.style.grain > 0.0) {
                        double g_offset = pixel_grain_dither(px, py) * node.style.grain;
                        fc.r = clamp01(fc.r + g_offset);
                        fc.g = clamp01(fc.g + g_offset);
                        fc.b = clamp01(fc.b + g_offset);
                    }
                    fc.a *= node.style.opacity;
                    blend_pixel_mode(dst, fc, fill_cov, node.style.blend_mode);
                }
            }

            if (has_stroke) {
                double sw = node.style.stroke_width;
                auto eval_stroke_d = [&](double dist_val) {
                    if (open_curve || node.style.stroke_align == StrokeAlign::Center) {
                        return std::abs(dist_val) - sw * 0.5;
                    } else if (node.style.stroke_align == StrokeAlign::Inner) {
                        return std::abs(dist_val + sw * 0.5) - sw * 0.5;
                    } else {
                        return std::abs(dist_val - sw * 0.5) - sw * 0.5;
                    }
                };

                double d_stroke = eval_stroke_d(d);
                double stroke_cov = 0.0;
                if (d_stroke < aa_width * 1.5) {
                    if (use_rgss && std::abs(d_stroke) <= aa_width * 1.5) {
                        for (int s = 0; s < 4; ++s) {
                            Vec2 ps = {(px + rgss_x[s]) * inv_sx, (py + rgss_y[s]) * inv_sy};
                            double ds = eval_stroke_d(evaluate_shape_sdf(node, ps));
                            stroke_cov += std::clamp(0.5 - ds / (aa_width * 0.75), 0.0, 1.0);
                        }
                        stroke_cov *= 0.25;
                    } else {
                        stroke_cov = std::clamp(0.5 - d_stroke / aa_width, 0.0, 1.0);
                    }
                    if (has_dashes) {
                        stroke_cov *= evaluate_dash_mask(node.style, arc_len, aa_width);
                    }
                }

                if (has_arrows) {
                    double d_arr = evaluate_node_arrowheads_sdf(node, p_center);
                    double arr_cov = std::clamp(0.5 - d_arr / aa_width, 0.0, 1.0);
                    stroke_cov = std::max(stroke_cov, arr_cov);
                }

                stroke_cov *= clip_cov;
                if (stroke_cov > 0.0) {
                    Color sc = node.style.stroke.sample(p_center, node.bbox);
                    sc.a *= node.style.opacity;
                    blend_pixel_mode(dst, sc, stroke_cov, node.style.blend_mode);
                }
            }
        }
    }
}

Result render_scene_ex(const Scene& scene, RasterImage& out_image, const RenderOptions& opts) {
    uint32_t w = opts.override_width > 0 ? opts.override_width : scene.width;
    uint32_t h = opts.override_height > 0 ? opts.override_height : scene.height;
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        return {false, "Invalid output dimensions", 0, 0};
    }

    out_image.width = w;
    out_image.height = h;
    out_image.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    if (scene.has_background) {
        uint8_t br = static_cast<uint8_t>(std::round(clamp01(scene.background.r) * 255.0));
        uint8_t bg = static_cast<uint8_t>(std::round(clamp01(scene.background.g) * 255.0));
        uint8_t bb = static_cast<uint8_t>(std::round(clamp01(scene.background.b) * 255.0));
        uint8_t ba = static_cast<uint8_t>(std::round(clamp01(scene.background.a) * 255.0));
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            out_image.rgba[i * 4 + 0] = br;
            out_image.rgba[i * 4 + 1] = bg;
            out_image.rgba[i * 4 + 2] = bb;
            out_image.rgba[i * 4 + 3] = ba;
        }
    }

    double sx = static_cast<double>(w) / std::max(1u, scene.width);
    double sy = static_cast<double>(h) / std::max(1u, scene.height);
    double inv_sx = 1.0 / sx;
    double inv_sy = 1.0 / sy;

    std::vector<const ShapeNode*> leaves;
    for (const auto& node : scene.nodes) {
        flatten_leaf_nodes(node, leaves);
    }

    constexpr int TILE_SIZE = 64;
    std::vector<TileBin> bins;
    for (int ty = 0; ty < static_cast<int>(h); ty += TILE_SIZE) {
        for (int tx = 0; tx < static_cast<int>(w); tx += TILE_SIZE) {
            TileRect tr = {
                tx,
                ty,
                std::min(static_cast<int>(w) - 1, tx + TILE_SIZE - 1),
                std::min(static_cast<int>(h) - 1, ty + TILE_SIZE - 1)
            };
            double wx0 = tr.x0 * inv_sx;
            double wy0 = tr.y0 * inv_sy;
            double wx1 = (tr.x1 + 1) * inv_sx;
            double wy1 = (tr.y1 + 1) * inv_sy;

            TileBin bin;
            bin.rect = tr;
            for (const ShapeNode* leaf : leaves) {
                if (leaf->render_bounds.overlaps(wx0, wy0, wx1, wy1)) {
                    bin.active_nodes.push_back(leaf);
                }
            }
            if (!bin.active_nodes.empty()) {
                bins.push_back(std::move(bin));
            }
        }
    }

    if (bins.empty()) return {true, "", 0, 0};

    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    int num_threads = opts.num_threads > 0 ? opts.num_threads : std::max(1, std::min(hw_threads, 8));
    if (bins.size() < 4) num_threads = 1;

    std::atomic<size_t> next_bin{0};
    auto worker = [&]() {
        while (true) {
            size_t idx = next_bin.fetch_add(1, std::memory_order_relaxed);
            if (idx >= bins.size()) break;
            const TileBin& bin = bins[idx];
            for (const ShapeNode* leaf : bin.active_nodes) {
                render_shape_node_tile(scene, *leaf, out_image, sx, sy, opts.ssaa_samples, bin.rect);
            }
        }
    };

    if (num_threads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            pool.emplace_back(worker);
        }
        for (auto& th : pool) th.join();
    }

    return {true, "", 0, 0};
}

Result render_scene(const Scene& scene, RasterImage& out_image, uint32_t override_width, uint32_t override_height) {
    RenderOptions opts;
    opts.override_width = override_width;
    opts.override_height = override_height;
    opts.ssaa_samples = 1;
    opts.num_threads = 0;
    return render_scene_ex(scene, out_image, opts);
}

Result benchmark_scene(const std::string& source, int iterations, const RenderOptions& opts, BenchmarkStats& out_stats,
                       const std::string& base_dir) {
    iterations = std::clamp(iterations, 1, 1000);
    double sum_parse = 0.0;
    double sum_render = 0.0;
    Scene scene;
    RasterImage img;

    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        Result pr = parse_and_evaluate(source, scene, 0.25, base_dir);
        if (!pr.ok) return pr;
        auto t1 = std::chrono::steady_clock::now();
        Result rr = render_scene_ex(scene, img, opts);
        if (!rr.ok) return rr;
        auto t2 = std::chrono::steady_clock::now();

        sum_parse += std::chrono::duration<double, std::milli>(t1 - t0).count();
        sum_render += std::chrono::duration<double, std::milli>(t2 - t1).count();
    }

    out_stats.parse_layout_ms = sum_parse / iterations;
    out_stats.render_ms = sum_render / iterations;
    out_stats.total_ms = out_stats.parse_layout_ms + out_stats.render_ms;
    double mpix = (static_cast<double>(img.width) * img.height) / 1e6;
    out_stats.megapixels_per_sec = out_stats.render_ms > 1e-6 ? (mpix / (out_stats.render_ms * 0.001)) : 0.0;

    std::vector<const ShapeNode*> leaves;
    for (const auto& n : scene.nodes) flatten_leaf_nodes(n, leaves);
    out_stats.leaf_nodes = leaves.size();

    size_t active_bins = 0;
    for (uint32_t ty = 0; ty < img.height; ty += 64) {
        for (uint32_t tx = 0; tx < img.width; tx += 64) {
            bool any = false;
            for (const auto* l : leaves) {
                if (l->render_bounds.overlaps(tx, ty, tx + 64, ty + 64)) { any = true; break; }
            }
            if (any) active_bins++;
        }
    }
    out_stats.active_tile_bins = active_bins;
    return {true, "", 0, 0};
}

// ============================================================================
// 4. Lossless Compressed PNG Encoder (Paeth Filter + LZ77 + Fixed Huffman)
// ============================================================================

static uint32_t crc32_compute(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t adler32_compute(const uint8_t* data, size_t len) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

static void append_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void append_png_chunk(std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& payload) {
    append_be32(png, static_cast<uint32_t>(payload.size()));
    size_t crc_start = png.size();
    png.push_back(static_cast<uint8_t>(type[0]));
    png.push_back(static_cast<uint8_t>(type[1]));
    png.push_back(static_cast<uint8_t>(type[2]));
    png.push_back(static_cast<uint8_t>(type[3]));
    png.insert(png.end(), payload.begin(), payload.end());
    uint32_t crc = crc32_compute(png.data() + crc_start, payload.size() + 4);
    append_be32(png, crc);
}

static inline uint8_t paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t bit_buf = 0;
    int bit_cnt = 0;

    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}

    void write_bits(uint32_t val, int nbits) {
        bit_buf |= (val << bit_cnt);
        bit_cnt += nbits;
        while (bit_cnt >= 8) {
            out.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
            bit_buf >>= 8;
            bit_cnt -= 8;
        }
    }

    void write_huffman(uint32_t code, int nbits) {
        uint32_t rev = 0;
        for (int i = 0; i < nbits; ++i) {
            rev = (rev << 1) | ((code >> i) & 1u);
        }
        write_bits(rev, nbits);
    }

    void flush_byte_align() {
        if (bit_cnt > 0) {
            out.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
            bit_buf = 0;
            bit_cnt = 0;
        }
    }
};

static void emit_fixed_lit_len(BitWriter& bw, int sym) {
    if (sym <= 143) {
        bw.write_huffman(0x30 + sym, 8);
    } else if (sym <= 255) {
        bw.write_huffman(0x190 + (sym - 144), 9);
    } else if (sym <= 279) {
        bw.write_huffman(sym - 256, 7);
    } else {
        bw.write_huffman(0xC0 + (sym - 280), 8);
    }
}

static void emit_fixed_length_and_dist(BitWriter& bw, int length, int dist) {
    static const int len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const int len_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    int l_idx = 0;
    for (int i = 28; i >= 0; --i) {
        if (length >= len_base[i]) { l_idx = i; break; }
    }
    emit_fixed_lit_len(bw, 257 + l_idx);
    if (len_extra[l_idx] > 0) {
        bw.write_bits(static_cast<uint32_t>(length - len_base[l_idx]), len_extra[l_idx]);
    }

    static const int dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const int dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    int d_idx = 0;
    for (int i = 29; i >= 0; --i) {
        if (dist >= dist_base[i]) { d_idx = i; break; }
    }
    bw.write_huffman(static_cast<uint32_t>(d_idx), 5);
    if (dist_extra[d_idx] > 0) {
        bw.write_bits(static_cast<uint32_t>(dist - dist_base[d_idx]), dist_extra[d_idx]);
    }
}

static void compress_zlib_fixed_huffman(const std::vector<uint8_t>& input, std::vector<uint8_t>& out_zlib) {
    out_zlib.clear();
    out_zlib.reserve(input.size() / 4 + 256);
    out_zlib.push_back(0x78);
    out_zlib.push_back(0x9C);

    BitWriter bw(out_zlib);
    bw.write_bits(1, 1);
    bw.write_bits(1, 2);

    constexpr int HASH_SIZE = 32768;
    std::vector<int> head(HASH_SIZE, -1);
    std::vector<int> prev(input.size(), -1);

    auto hash3 = [&](size_t pos) -> uint32_t {
        uint32_t v = (static_cast<uint32_t>(input[pos]) << 16) |
                     (static_cast<uint32_t>(input[pos + 1]) << 8) |
                     static_cast<uint32_t>(input[pos + 2]);
        return ((v * 2654435761u) >> 17) & (HASH_SIZE - 1);
    };

    size_t n = input.size();
    size_t i = 0;
    while (i < n) {
        int best_len = 0;
        int best_dist = 0;

        if (i + 3 <= n) {
            uint32_t h = hash3(i);
            int cand = head[h];
            prev[i] = cand;
            head[h] = static_cast<int>(i);

            int chain_steps = 0;
            while (cand >= 0 && (static_cast<int>(i) - cand) <= 32768 && chain_steps < 24) {
                size_t max_l = std::min<size_t>(258, n - i);
                if (best_len < static_cast<int>(max_l) &&
                    input[cand + best_len] == input[i + best_len]) {
                    size_t l = 0;
                    while (l < max_l && input[cand + l] == input[i + l]) {
                        ++l;
                    }
                    if (static_cast<int>(l) >= 3 && static_cast<int>(l) > best_len) {
                        best_len = static_cast<int>(l);
                        best_dist = static_cast<int>(i) - cand;
                        if (best_len == 258) break;
                    }
                }
                cand = prev[cand];
                chain_steps++;
            }
        }

        if (best_len >= 3) {
            emit_fixed_length_and_dist(bw, best_len, best_dist);
            for (int k = 1; k < best_len; ++k) {
                if (i + k + 3 <= n) {
                    uint32_t hk = hash3(i + k);
                    prev[i + k] = head[hk];
                    head[hk] = static_cast<int>(i + k);
                }
            }
            i += best_len;
        } else {
            emit_fixed_lit_len(bw, input[i]);
            ++i;
        }
    }

    emit_fixed_lit_len(bw, 256);
    bw.flush_byte_align();

    uint32_t adler = adler32_compute(input.data(), input.size());
    append_be32(out_zlib, adler);
}

Result encode_png(const RasterImage& image, std::vector<uint8_t>& out_png_bytes) {
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4) {
        return {false, "Invalid RasterImage buffer for PNG encoding", 0, 0};
    }

    out_png_bytes.clear();
    out_png_bytes.reserve(image.rgba.size() / 4 + 1024);
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (int i = 0; i < 8; ++i) out_png_bytes.push_back(sig[i]);

    std::vector<uint8_t> ihdr;
    append_be32(ihdr, image.width);
    append_be32(ihdr, image.height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    append_png_chunk(out_png_bytes, "IHDR", ihdr);

    size_t row_bytes = static_cast<size_t>(image.width) * 4;
    std::vector<uint8_t> filtered_scanlines;
    filtered_scanlines.reserve((row_bytes + 1) * image.height);

    for (uint32_t y = 0; y < image.height; ++y) {
        const uint8_t* row = image.rgba.data() + static_cast<size_t>(y) * row_bytes;
        const uint8_t* prev_row = (y > 0) ? (row - row_bytes) : nullptr;
        filtered_scanlines.push_back(4);
        for (size_t x = 0; x < row_bytes; ++x) {
            int a = (x >= 4) ? row[x - 4] : 0;
            int b = prev_row ? prev_row[x] : 0;
            int c = (prev_row && x >= 4) ? prev_row[x - 4] : 0;
            uint8_t pred = paeth_predictor(a, b, c);
            filtered_scanlines.push_back(static_cast<uint8_t>(row[x] - pred));
        }
    }

    std::vector<uint8_t> zlib_stream;
    compress_zlib_fixed_huffman(filtered_scanlines, zlib_stream);

    append_png_chunk(out_png_bytes, "IDAT", zlib_stream);
    append_png_chunk(out_png_bytes, "IEND", {});
    return {true, "", 0, 0};
}

Result write_png_file(const RasterImage& image, const std::string& path) {
    std::vector<uint8_t> png;
    Result r = encode_png(image, png);
    if (!r.ok) return r;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return {false, "Cannot open output PNG file: " + path, 0, 0};
    ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return {true, "", 0, 0};
}

Result write_ppm_file(const RasterImage& image, const std::string& path) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return {false, "Cannot open output PPM file: " + path, 0, 0};
    ofs << "P6\n" << image.width << " " << image.height << "\n255\n";
    for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; ++i) {
        uint8_t rgb[3] = {image.rgba[i * 4 + 0], image.rgba[i * 4 + 1], image.rgba[i * 4 + 2]};
        ofs.write(reinterpret_cast<const char*>(rgb), 3);
    }
    return {true, "", 0, 0};
}

// ============================================================================
// 5. Animated GIF89a (LZW) & Sprite-Sheet Animation Exporters
// ============================================================================

static void write_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

static void gif_lzw_encode(const std::vector<uint8_t>& indexed, std::vector<uint8_t>& gif_stream) {
    gif_stream.push_back(8);

    std::vector<uint8_t> sub_block;
    sub_block.reserve(255);
    uint32_t bit_buf = 0;
    int bit_cnt = 0;

    auto emit_code = [&](uint32_t code, int width) {
        bit_buf |= (code << bit_cnt);
        bit_cnt += width;
        while (bit_cnt >= 8) {
            sub_block.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
            bit_buf >>= 8;
            bit_cnt -= 8;
            if (sub_block.size() == 255) {
                gif_stream.push_back(255);
                gif_stream.insert(gif_stream.end(), sub_block.begin(), sub_block.end());
                sub_block.clear();
            }
        }
    };

    std::vector<int> dict(4096 * 256, -1);
    int next_code = 258;
    int code_size = 9;

    emit_code(256, code_size);
    if (!indexed.empty()) {
        int prefix = indexed[0];
        for (size_t i = 1; i < indexed.size(); ++i) {
            uint8_t k = indexed[i];
            int entry = dict[prefix * 256 + k];
            if (entry != -1) {
                prefix = entry;
            } else {
                emit_code(static_cast<uint32_t>(prefix), code_size);
                if (next_code < 4096) {
                    dict[prefix * 256 + k] = next_code;
                    if (next_code == (1 << code_size) && code_size < 12) {
                        code_size++;
                    }
                    next_code++;
                } else {
                    emit_code(256, code_size);
                    std::fill(dict.begin(), dict.end(), -1);
                    next_code = 258;
                    code_size = 9;
                }
                prefix = k;
            }
        }
        emit_code(static_cast<uint32_t>(prefix), code_size);
    }

    emit_code(257, code_size);
    if (bit_cnt > 0) {
        sub_block.push_back(static_cast<uint8_t>(bit_buf & 0xFFu));
    }
    if (!sub_block.empty()) {
        gif_stream.push_back(static_cast<uint8_t>(sub_block.size()));
        gif_stream.insert(gif_stream.end(), sub_block.begin(), sub_block.end());
    }
    gif_stream.push_back(0);
}

Result render_animation_gif(const std::string& source, const std::string& path,
                            double duration_sec, int fps, const RenderOptions& opts,
                            const std::string& base_dir) {
    fps = std::clamp(fps, 1, 60);
    int total_frames = std::max(1, static_cast<int>(std::round(duration_sec * fps)));
    uint16_t delay_cs = static_cast<uint16_t>(std::max(2, static_cast<int>(std::round(100.0 / fps))));

    Scene s0;
    Result pr = parse_and_evaluate(source, s0, 0.0, base_dir);
    if (!pr.ok) return pr;

    RasterImage f0;
    Result rr = render_scene_ex(s0, f0, opts);
    if (!rr.ok) return rr;

    uint16_t w = static_cast<uint16_t>(f0.width);
    uint16_t h = static_cast<uint16_t>(f0.height);

    uint8_t palette[256][3];
    int idx = 0;
    for (int r = 0; r < 6; ++r) {
        for (int g = 0; g < 6; ++g) {
            for (int b = 0; b < 6; ++b) {
                palette[idx][0] = static_cast<uint8_t>(r * 51);
                palette[idx][1] = static_cast<uint8_t>(g * 51);
                palette[idx][2] = static_cast<uint8_t>(b * 51);
                idx++;
            }
        }
    }
    for (int k = 0; idx < 256; ++k, ++idx) {
        uint8_t v = static_cast<uint8_t>(std::round((k / 39.0) * 255.0));
        palette[idx][0] = palette[idx][1] = palette[idx][2] = v;
    }

    std::vector<uint8_t> gif;
    gif.reserve(w * h * total_frames / 2 + 2048);

    const char* hdr = "GIF89a";
    gif.insert(gif.end(), hdr, hdr + 6);
    write_le16(gif, w);
    write_le16(gif, h);
    gif.push_back(0xF7);
    gif.push_back(0);
    gif.push_back(0);

    for (int i = 0; i < 256; ++i) {
        gif.push_back(palette[i][0]);
        gif.push_back(palette[i][1]);
        gif.push_back(palette[i][2]);
    }

    const uint8_t netscape_ext[19] = {
        0x21, 0xFF, 0x0B,
        'N','E','T','S','C','A','P','E','2','.','0',
        0x03, 0x01, 0x00, 0x00, 0x00
    };
    gif.insert(gif.end(), netscape_ext, netscape_ext + 19);

    std::vector<uint8_t> indexed(static_cast<size_t>(w) * h);

    for (int f = 0; f < total_frames; ++f) {
        double t = (total_frames > 1) ? (static_cast<double>(f) / fps) : 0.0;
        RasterImage frame_img;
        if (f == 0) {
            frame_img = std::move(f0);
        } else {
            Scene sf;
            Result pfr = parse_and_evaluate(source, sf, t, base_dir);
            if (!pfr.ok) return pfr;
            Result rfr = render_scene_ex(sf, frame_img, opts);
            if (!rfr.ok) return rfr;
        }

        static const int bayer2x2[4] = {-12, 4, 12, -4};
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                size_t p = (static_cast<size_t>(y) * w + x) * 4;
                int dither = bayer2x2[((y & 1) << 1) | (x & 1)];
                int r = std::clamp(static_cast<int>(frame_img.rgba[p + 0]) + dither, 0, 255);
                int g = std::clamp(static_cast<int>(frame_img.rgba[p + 1]) + dither, 0, 255);
                int b = std::clamp(static_cast<int>(frame_img.rgba[p + 2]) + dither, 0, 255);

                if (std::abs(r - g) <= 6 && std::abs(g - b) <= 6) {
                    int gray_idx = static_cast<int>(std::round(((r + g + b) / 3.0) * 39.0 / 255.0));
                    indexed[y * w + x] = static_cast<uint8_t>(216 + std::clamp(gray_idx, 0, 39));
                } else {
                    int ri = (r * 5 + 127) / 255;
                    int gi = (g * 5 + 127) / 255;
                    int bi = (b * 5 + 127) / 255;
                    indexed[y * w + x] = static_cast<uint8_t>(ri * 36 + gi * 6 + bi);
                }
            }
        }

        gif.push_back(0x21);
        gif.push_back(0xF9);
        gif.push_back(0x04);
        gif.push_back(0x04);
        write_le16(gif, delay_cs);
        gif.push_back(0x00);
        gif.push_back(0x00);

        gif.push_back(0x2C);
        write_le16(gif, 0);
        write_le16(gif, 0);
        write_le16(gif, w);
        write_le16(gif, h);
        gif.push_back(0x00);

        gif_lzw_encode(indexed, gif);
    }

    gif.push_back(0x3B);

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return {false, "Cannot open output GIF file: " + path, 0, 0};
    ofs.write(reinterpret_cast<const char*>(gif.data()), static_cast<std::streamsize>(gif.size()));
    return {true, "", 0, 0};
}

Result render_animation_spritesheet(const std::string& source, const std::string& path,
                                    double duration_sec, int fps, const RenderOptions& opts,
                                    const std::string& base_dir) {
    fps = std::clamp(fps, 1, 60);
    int total_frames = std::max(1, static_cast<int>(std::round(duration_sec * fps)));

    Scene s0;
    Result pr = parse_and_evaluate(source, s0, 0.0, base_dir);
    if (!pr.ok) return pr;

    RasterImage f0;
    Result rr = render_scene_ex(s0, f0, opts);
    if (!rr.ok) return rr;

    uint32_t fw = f0.width;
    uint32_t fh = f0.height;
    RasterImage sheet;
    sheet.width = fw * total_frames;
    sheet.height = fh;
    sheet.rgba.assign(static_cast<size_t>(sheet.width) * sheet.height * 4, 0);

    for (int f = 0; f < total_frames; ++f) {
        double t = (total_frames > 1) ? (static_cast<double>(f) / fps) : 0.0;
        RasterImage frame_img;
        if (f == 0) {
            frame_img = std::move(f0);
        } else {
            Scene sf;
            Result pfr = parse_and_evaluate(source, sf, t, base_dir);
            if (!pfr.ok) return pfr;
            Result rfr = render_scene_ex(sf, frame_img, opts);
            if (!rfr.ok) return rfr;
        }

        for (uint32_t y = 0; y < fh; ++y) {
            const uint8_t* src_row = &frame_img.rgba[static_cast<size_t>(y) * fw * 4];
            uint8_t* dst_row = &sheet.rgba[(static_cast<size_t>(y) * sheet.width + static_cast<size_t>(f) * fw) * 4];
            std::memcpy(dst_row, src_row, static_cast<size_t>(fw) * 4);
        }
    }

    return write_png_file(sheet, path);
}

} // namespace mesh
