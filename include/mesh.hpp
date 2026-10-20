// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#ifndef MESH_HPP
#define MESH_HPP

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mesh {

constexpr const char* VERSION = "1.0.0";
constexpr int VERSION_MAJOR = 1;
constexpr int VERSION_MINOR = 0;
constexpr int VERSION_PATCH = 0;
constexpr double PI = 3.14159265358979323846;

inline double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    Vec2() = default;
    constexpr Vec2(double x_, double y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    Vec2 operator/(double s) const { return s != 0.0 ? Vec2{x / s, y / s} : Vec2{0.0, 0.0}; }

    double dot(const Vec2& o) const { return x * o.x + y * o.y; }
    double cross(const Vec2& o) const { return x * o.y - y * o.x; }
    double length_sq() const { return x * x + y * y; }
    double length() const { return std::sqrt(length_sq()); }
    Vec2 normalized() const {
        double l = length();
        return l > 1e-12 ? (*this / l) : Vec2{0.0, 0.0};
    }
};

struct BBox {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    bool valid = false;

    double width() const { return valid ? (max_x - min_x) : 0.0; }
    double height() const { return valid ? (max_y - min_y) : 0.0; }
    double center_x() const { return valid ? 0.5 * (min_x + max_x) : 0.0; }
    double center_y() const { return valid ? 0.5 * (min_y + max_y) : 0.0; }
    Vec2 center() const { return {center_x(), center_y()}; }

    void include(const Vec2& p) {
        if (!valid) {
            min_x = max_x = p.x;
            min_y = max_y = p.y;
            valid = true;
        } else {
            if (p.x < min_x) min_x = p.x;
            if (p.y < min_y) min_y = p.y;
            if (p.x > max_x) max_x = p.x;
            if (p.y > max_y) max_y = p.y;
        }
    }

    void expand(double r) {
        if (!valid) return;
        min_x -= r;
        min_y -= r;
        max_x += r;
        max_y += r;
    }

    void merge(const BBox& o) {
        if (!o.valid) return;
        include({o.min_x, o.min_y});
        include({o.max_x, o.max_y});
    }

    bool overlaps(double x0, double y0, double x1, double y1) const {
        if (!valid) return false;
        return !(max_x < x0 || min_x > x1 || max_y < y0 || min_y > y1);
    }
};

// Perceptually accurate color representation supporting sRGB, linear RGB, OKLab, and OKLCH.
struct Color {
    double r = 0.0; // sRGB [0, 1]
    double g = 0.0; // sRGB [0, 1]
    double b = 0.0; // sRGB [0, 1]
    double a = 1.0; // Alpha [0, 1]

    // Cached perceptual OKLab coordinates for fast gradient interpolation
    double ok_L = 0.0;
    double ok_a = 0.0;
    double ok_b = 0.0;

    static Color from_srgb(double r, double g, double b, double a = 1.0);
    static Color from_oklch(double L, double C, double h_deg, double a = 1.0);
    static Color from_oklab(double L, double a_lab, double b_lab, double alpha = 1.0);
    static Color from_hsl(double h_deg, double s, double l, double a = 1.0);
    static Color from_hex(const std::string& hex_str);

    // Perceptual interpolation & manipulation in OKLab space
    static Color lerp_oklab(const Color& c0, const Color& c1, double t);
    Color lighten(double delta_L) const;
    Color darken(double delta_L) const;
    Color with_alpha(double new_alpha) const;

    std::string to_css_rgba() const;
    std::string to_hex() const;
};

struct GradientStop {
    double offset = 0.0; // [0, 1]
    Color color;
};

struct DiffusionPoint {
    double u = 0.5; // Normalized [0, 1] or canvas coordinate if absolute
    double v = 0.5;
    Color color;
    double weight = 1.0;
};

enum class PaintKind : uint8_t {
    None = 0,
    Solid = 1,
    LinearGradient = 2,
    RadialGradient = 3,
    ConicGradient = 4,
    MeshGradient = 5,
    DiffusionGradient = 6,
    NoiseGradient = 7
};

struct PaintServer {
    PaintKind kind = PaintKind::None;
    std::string id;
    Color solid_color;

    // Linear / Radial / Conic parameters (normalized to shape bbox [0, 1] by default, or user space)
    Vec2 start = {0.0, 0.0};
    Vec2 end = {1.0, 1.0};
    Vec2 center = {0.5, 0.5};
    double radius = 0.5;
    double angle_deg = 0.0;
    bool user_space = false;
    std::vector<GradientStop> stops;

    // Mesh gradient (structured rows x cols grid of control points/colors)
    int rows = 2;
    int cols = 2;
    std::vector<DiffusionPoint> grid_points;

    // Freeform Diffusion gradient (arbitrary weighted points in [0, 1]^2)
    double falloff_exponent = 2.6;
    std::vector<DiffusionPoint> diffusion_points;

    // Procedural FBM Noise Gradient in OKLab space + Film Grain
    double noise_scale = 4.0;
    int noise_octaves = 4;
    double noise_persistence = 0.5;
    double noise_lacunarity = 2.0;
    double noise_seed = 0.0;
    double grain = 0.0;

    // Precomputed LUTs for real-time 120+ FPS sampling
    std::vector<Color> stop_lut;      // 256-entry OKLab-interpolated stop table
    std::vector<Color> diff_grid_lut; // 64x64 precomputed 2D diffusion/mesh table

    void build_luts();
    Color sample(const Vec2& p_world, const BBox& bbox) const;
};

enum class StrokeAlign : uint8_t {
    Center = 0,
    Inner = 1,
    Outer = 2
};

enum class LineCap : uint8_t {
    Butt = 0,
    Round = 1,
    Square = 2
};

enum class LineJoin : uint8_t {
    Miter = 0,
    Round = 1,
    Bevel = 2
};

enum class BooleanOp : uint8_t {
    Union = 0,
    Subtract = 1,
    Intersect = 2,
    SmoothUnion = 3
};

enum class TextAlign : uint8_t {
    Left = 0,
    Center = 1,
    Right = 2
};

// Font matching request. A face can be resolved explicitly from `src` or through
// the platform font database from `family` (weight/slant/stretch are honored).
enum class FontStyle : uint8_t {
    Normal = 0,
    Italic = 1,
    Oblique = 2
};

struct FontSpec {
    std::string id;
    std::string family;   // requested family name, e.g. "Segoe UI"
    std::string src;      // optional explicit .ttf/.otf/.ttc path
    std::string fallback; // optional fallback family for missing glyphs
    double weight = 400.0;
    FontStyle style = FontStyle::Normal;
    double stretch = 100.0; // percentage
    int face_index = 0;     // index inside a .ttc collection
};

enum class BlendMode : uint8_t {
    Normal = 0,
    Add = 1,
    Screen = 2,
    Multiply = 3,
    Overlay = 4
};

enum class ArrowMode : uint8_t {
    None = 0,
    End = 1,
    Start = 2,
    Both = 3
};

struct ShadowSpec {
    bool enabled = false;
    double dx = 0.0;
    double dy = 8.0;
    double blur = 16.0;
    Color color = Color::from_srgb(0, 0, 0, 0.45);
};

struct GlowSpec {
    bool enabled = false;
    double radius = 12.0;
    Color color = Color::from_srgb(1, 1, 1, 0.5);
};

struct Style {
    PaintServer fill;
    PaintServer stroke;
    double stroke_width = 0.0;
    StrokeAlign stroke_align = StrokeAlign::Center;
    LineCap line_cap = LineCap::Round;
    LineJoin line_join = LineJoin::Round;
    std::vector<double> dash_array;
    double dash_offset = 0.0;
    double dash_period = 0.0; // Precomputed sum of dash_array
    double opacity = 1.0;
    double grain = 0.0; // Perceptual film grain dithering [0, 1]
    ArrowMode arrow_mode = ArrowMode::None;
    double arrow_size = 11.0;
    BlendMode blend_mode = BlendMode::Normal;
    std::string clip_id; // Optional `@id` analytical SDF clipping target
    ShadowSpec shadow;
    GlowSpec glow;
};

struct Transform2D {
    // 2D affine matrix:
    // [ a  c  tx ]
    // [ b  d  ty ]
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, tx = 0.0, ty = 0.0;

    static Transform2D identity() { return {}; }
    bool is_identity() const {
        return std::abs(a - 1.0) <= 1e-12 && std::abs(b) <= 1e-12 &&
               std::abs(c) <= 1e-12 && std::abs(d - 1.0) <= 1e-12 &&
               std::abs(tx) <= 1e-12 && std::abs(ty) <= 1e-12;
    }
    static Transform2D translate(double x, double y) {
        Transform2D t;
        t.tx = x;
        t.ty = y;
        return t;
    }
    static Transform2D scale(double sx, double sy, Vec2 pivot = {0.0, 0.0}) {
        Transform2D t;
        t.a = sx;
        t.d = sy;
        t.tx = pivot.x * (1.0 - sx);
        t.ty = pivot.y * (1.0 - sy);
        return t;
    }
    static Transform2D rotate_deg(double deg, Vec2 pivot = {0.0, 0.0}) {
        double rad = deg * PI / 180.0;
        double cs = std::cos(rad);
        double sn = std::sin(rad);
        Transform2D r;
        r.a = cs;
        r.b = sn;
        r.c = -sn;
        r.d = cs;
        r.tx = pivot.x - cs * pivot.x + sn * pivot.y;
        r.ty = pivot.y - sn * pivot.x - cs * pivot.y;
        return r;
    }

    Transform2D multiply(const Transform2D& o) const {
        Transform2D res;
        res.a = a * o.a + c * o.b;
        res.b = b * o.a + d * o.b;
        res.c = a * o.c + c * o.d;
        res.d = b * o.c + d * o.d;
        res.tx = a * o.tx + c * o.ty + tx;
        res.ty = b * o.tx + d * o.ty + ty;
        return res;
    }

    Vec2 apply(const Vec2& p) const {
        return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }

    Transform2D inverse() const {
        double det = a * d - b * c;
        if (std::abs(det) < 1e-12) return identity();
        double inv = 1.0 / det;
        Transform2D res;
        res.a = d * inv;
        res.b = -b * inv;
        res.c = -c * inv;
        res.d = a * inv;
        res.tx = (c * ty - d * tx) * inv;
        res.ty = (b * tx - a * ty) * inv;
        return res;
    }
};

enum class PathVerb : uint8_t {
    MoveTo = 0,
    LineTo = 1,
    QuadTo = 2,
    CubicTo = 3,
    ArcTo = 4,
    BulgeTo = 5,
    Close = 6
};

struct PathSegment {
    PathVerb verb = PathVerb::MoveTo;
    Vec2 p1; // Target point (or control point 1)
    Vec2 p2; // Control point 2 or target point
    Vec2 p3; // Target point for CubicTo
    double radius = 0.0;
    double bulge = 0.0;
    bool clockwise = true;
    bool large_arc = false;
};

struct FlattenedSubpath {
    std::vector<Vec2> points;
    std::vector<double> cumulative_lengths; // Precomputed arc-length table
    bool closed = false;
};

enum class ShapeKind : uint8_t {
    Rect = 0,
    Circle = 1,
    Ellipse = 2,
    Line = 3,
    Polygon = 4,
    Polyline = 5,
    Spline = 6,
    Path = 7,
    Boolean = 8,
    Text = 9,
    Group = 10
};

struct ShapeNode {
    ShapeKind kind = ShapeKind::Rect;
    std::string id;
    Style style;
    Transform2D transform;
    Transform2D inv_transform; // Precomputed inverse affine matrix
    double scale_factor = 1.0; // Precomputed metric scale factor
    BBox bbox;                 // World-space bounding box registered for `@id.*` relational queries
    BBox render_bounds;        // Expanded world-space bounds including stroke, shadow, and glow for tile binning

    // Rect parameters
    double x = 0.0;
    double y = 0.0;
    double width = 100.0;
    double height = 100.0;
    double corner_radius = 0.0;
    double corner_radii[4] = {0.0, 0.0, 0.0, 0.0}; // TL, TR, BR, BL

    // Circle / Ellipse parameters
    Vec2 center = {0.0, 0.0};
    double radius = 50.0;
    double rx = 50.0;
    double ry = 50.0;

    // Line parameters
    Vec2 from = {0.0, 0.0};
    Vec2 to = {100.0, 100.0};

    // Polygon / Polyline / Spline parameters
    std::vector<Vec2> points;
    std::vector<double> point_lengths; // Cumulative arc lengths for Polyline/Polygon
    double tension = 0.5;
    bool closed = false;

    // Path parameters & resolved cubic/flattened geometry
    std::vector<PathSegment> path_segments;
    std::vector<FlattenedSubpath> subpaths;

    // Boolean CSG & Group children
    BooleanOp bool_op = BooleanOp::Union;
    double smoothness = 0.0; // For smooth_union(k)
    std::vector<ShapeNode> children;

    // Text parameters
    Vec2 text_pos = {0.0, 0.0};
    std::string text_content;
    double font_size = 18.0;
    double font_weight = 400.0; // 300 = light, 400 = normal, 600 = semibold, 700 = bold
    TextAlign text_align = TextAlign::Left;
    double letter_spacing = 0.0;
    double word_spacing = 0.0;

    // Font selection / typography
    std::string font_id;       // reference to a top-level `font` declaration
    std::string font_family;   // inline family override
    std::string font_src;      // inline explicit face file
    std::string font_fallback; // inline fallback family
    std::string font_features; // OpenType features, e.g. "kern,liga,ss01"
    std::string font_language; // BCP-47 language tag for shaping
    FontStyle font_style = FontStyle::Normal;
    double font_stretch = 100.0;
    bool font_weight_explicit = false;
    bool font_style_explicit = false;
    bool font_stretch_explicit = false;
    double line_height = 1.25; // multiplier of font size
    double max_width = 0.0;    // 0 = no wrapping
    bool wrap = false;         // wrap at max_width when true
    bool baseline_anchor = false; // true = text_pos.y is the first baseline

    // Resolved font + laid-out glyph geometry (world/local canvas coords)
    std::string resolved_font_family;
    std::string resolved_font_path;
    double text_width = 0.0;
    double text_ascent = 0.0;
    double text_descent = 0.0;
    double text_line_height = 0.0;
    int text_line_count = 0;
    std::vector<FlattenedSubpath> text_subpaths;
};

struct Scene {
    std::string name = "untitled";
    uint32_t width = 800;
    uint32_t height = 600;
    Color background = Color::from_srgb(1.0, 1.0, 1.0, 0.0);
    bool has_background = false;
    double eval_time = 0.0;

    std::map<std::string, double> variables;
    std::map<std::string, Color> color_variables; // `let primary = oklch(...)`
    std::map<std::string, Vec2> point_variables;  // `let origin = (100, 200)`
    std::map<std::string, PaintServer> paint_servers;
    std::map<std::string, FontSpec> fonts;        // `font "ui" { ... }`
    std::string base_dir;                         // source directory for relative font files
    std::string default_font_family = "Noto Sans";
    std::string default_font_fallback = "DejaVu Sans";
    std::map<std::string, ShapeNode> defs;        // Reusable `def` component templates
    std::map<std::string, BBox> node_bboxes;      // Resolved `@id` bounding boxes
    std::map<std::string, ShapeNode> named_nodes; // Resolved `@id` nodes for `clip: @id` SDF lookup
    std::vector<ShapeNode> nodes;
};

struct RenderOptions {
    uint32_t override_width = 0;
    uint32_t override_height = 0;
    int ssaa_samples = 1; // 1 = Analytical SDF AA, 4 = 4x4 Rotated-Grid Supersampling + SDF
    int num_threads = 0;  // 0 = auto (std::thread::hardware_concurrency())
};

struct RasterImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // 4 bytes per pixel, row-major
};

struct BenchmarkStats {
    double parse_layout_ms = 0.0;
    double render_ms = 0.0;
    double total_ms = 0.0;
    double megapixels_per_sec = 0.0;
    size_t active_tile_bins = 0;
    size_t leaf_nodes = 0;
};

struct Result {
    bool ok = true;
    std::string error;
    int line = 0;
    int column = 0;
};

// Core Engine Pipeline Functions (v1.0.0 Production Release)
Result parse_and_evaluate(const std::string& source, Scene& out_scene, double time_sec = 0.0,
                          const std::string& base_dir = "");
Result render_scene(const Scene& scene, RasterImage& out_image, uint32_t override_width = 0, uint32_t override_height = 0);
Result render_scene_ex(const Scene& scene, RasterImage& out_image, const RenderOptions& opts);
Result benchmark_scene(const std::string& source, int iterations, const RenderOptions& opts, BenchmarkStats& out_stats,
                       const std::string& base_dir = "");

// Lossless Compressed PNG (Paeth/Sub filter + LZ77 + Fixed Huffman Deflate) & PPM Encoders
Result encode_png(const RasterImage& image, std::vector<uint8_t>& out_png_bytes);
Result write_png_file(const RasterImage& image, const std::string& path);
Result write_ppm_file(const RasterImage& image, const std::string& path);

// Multi-Frame Animation Exporters (Animated GIF89a with LZW + Sprite-Sheet PNG)
Result render_animation_gif(const std::string& source, const std::string& path, double duration_sec, int fps, const RenderOptions& opts,
                            const std::string& base_dir = "");
Result render_animation_spritesheet(const std::string& source, const std::string& path, double duration_sec, int fps, const RenderOptions& opts,
                                    const std::string& base_dir = "");

// Native H.264/MP4 animation export. This backend is optional: it is compiled in
// only when FFmpeg development libraries are available. `mp4_export_supported()`
// reports that; when false, `render_animation_mp4` returns an actionable error.
bool mp4_export_supported();
Result render_animation_mp4(const std::string& source, const std::string& path, double duration_sec, int fps, const RenderOptions& opts,
                            const std::string& base_dir = "");

// Binary Wire Format (.meshb) Serialization & Deserialization
Result encode_meshb(const Scene& scene, std::vector<uint8_t>& out_bytes);
Result decode_meshb(const std::vector<uint8_t>& bytes, Scene& out_scene);
Result write_meshb_file(const Scene& scene, const std::string& path);
Result read_meshb_file(const std::string& path, Scene& out_scene);
Result decompile_scene_to_mesh(const Scene& scene, std::string& out_mesh_source);

// Legacy SVG -> .mesh Migrator / Importer
Result import_svg_to_mesh(const std::string& svg_content, std::string& out_mesh_source);

// Browser & GPU Shader Compilers
Result compile_to_html(const Scene& scene, const std::string& original_source, std::string& out_html);
Result compile_to_svg(const Scene& scene, std::string& out_svg);
Result compile_to_wgsl(const Scene& scene, std::string& out_wgsl);
Result compile_to_glsl(const Scene& scene, std::string& out_glsl);
Result format_mesh_source(const std::string& source, std::string& out_formatted,
                          const std::string& base_dir = "");

// Text/font pipeline. `layout_text_node` resolves the font, shapes the text and
// replaces `node.text_subpaths` with real vector outlines. It is called by the
// parser and can be called again after editing a Scene programmatically.
Result layout_text_node(ShapeNode& node, const Scene& scene, const std::string& base_dir = "");
Result layout_scene_text(Scene& scene);

// Helper geometry & SDF evaluators exposed for backends and tests
double evaluate_shape_sdf(const ShapeNode& node, const Vec2& p_world, double* out_arc_len = nullptr);
std::vector<PathSegment> spline_to_cubics(const std::vector<Vec2>& pts, double tension, bool closed);
void recompute_scene_Caches(Scene& scene);
double fbm_noise_2d(double x, double y, int octaves, double persistence, double lacunarity, double seed);

} // namespace mesh

#endif // MESH_HPP
