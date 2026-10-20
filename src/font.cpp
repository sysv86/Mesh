// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "mesh_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Optional dependency detection. A build system may define MESH_HAVE_FREETYPE,
// MESH_HAVE_HARFBUZZ and MESH_HAVE_FONTCONFIG. When it does not, headers that
// are visible on the include path are used automatically so a plain
// `pkg-config --cflags freetype2 harfbuzz` build works out of the box.
// ---------------------------------------------------------------------------

#if !defined(MESH_NO_FONT_CORE)
#  if !defined(MESH_HAVE_FREETYPE) && defined(__has_include)
#    if __has_include(<ft2build.h>)
#      define MESH_HAVE_FREETYPE 1
#    endif
#  endif

#  if !defined(MESH_HAVE_HARFBUZZ) && defined(__has_include)
#    if __has_include(<hb.h>)
#      define MESH_HAVE_HARFBUZZ 1
#    endif
#  endif

#  if !defined(MESH_HAVE_FONTCONFIG) && defined(__has_include)
#    if __has_include(<fontconfig/fontconfig.h>)
#      define MESH_HAVE_FONTCONFIG 1
#    endif
#  endif
#endif

#if !defined(MESH_NO_FONT_CORE) && defined(MESH_HAVE_FREETYPE) && defined(MESH_HAVE_HARFBUZZ)
#  define MESH_FONT_CORE 1
#endif

#if defined(MESH_FONT_CORE)
#  include <ft2build.h>
#  include FT_FREETYPE_H
#  include FT_OUTLINE_H
#  include <hb.h>
#  include <hb-ot.h>
#  if defined(MESH_HAVE_FONTCONFIG)
#    include <fontconfig/fontconfig.h>
#  endif
#endif

namespace mesh {
namespace {

constexpr double kFlattenTolerancePx = 0.2;

#if defined(MESH_FONT_CORE)

// ------------------------------- UTF-8 -------------------------------------

std::size_t utf8_decode(const std::string& s, std::size_t i, std::size_t end,
                        std::uint32_t& cp) {
    if (i >= end) {
        cp = 0;
        return i;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        cp = c0;
        return i + 1;
    }

    std::size_t len = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((c0 & 0xE0) == 0xC0) {
        len = 2;
        value = c0 & 0x1Fu;
        minimum = 0x80;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
        value = c0 & 0x0Fu;
        minimum = 0x800;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
        value = c0 & 0x07u;
        minimum = 0x10000;
    } else {
        cp = 0xFFFD;
        return i + 1;
    }

    if (i + len > end) {
        cp = 0xFFFD;
        return i + 1;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return i + 1;
        }
        value = (value << 6) | (ck & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        cp = 0xFFFD;
        return i + 1;
    }
    cp = value;
    return i + len;
}

bool is_rtl_codepoint(std::uint32_t cp) {
    return (cp >= 0x0590 && cp <= 0x05FF) || // Hebrew
           (cp >= 0x0600 && cp <= 0x06FF) || // Arabic
           (cp >= 0x0700 && cp <= 0x074F) || // Syriac
           (cp >= 0x0750 && cp <= 0x077F) || // Arabic Supplement
           (cp >= 0x0780 && cp <= 0x07BF) || // Thaana
           (cp >= 0x07C0 && cp <= 0x07FF) || // NKo
           (cp >= 0x0800 && cp <= 0x085F) || // Samaritan / Mandaic
           (cp >= 0x0860 && cp <= 0x08FF) || // Arabic Extended
           (cp >= 0xFB1D && cp <= 0xFDFF) || // Hebrew/Arabic presentation
           (cp >= 0xFE70 && cp <= 0xFEFF) || // Arabic presentation B
           (cp >= 0x10800 && cp <= 0x10FFF) ||
           (cp >= 0x1E800 && cp <= 0x1EFFF) ||
           (cp >= 0x1EE00 && cp <= 0x1EEFF);
}

bool is_ltr_strong_codepoint(std::uint32_t cp) {
    if (is_rtl_codepoint(cp)) return false;
    return (cp >= 0x0041 && cp <= 0x005A) ||
           (cp >= 0x0061 && cp <= 0x007A) ||
           (cp >= 0x00C0 && cp <= 0x02B8) ||
           (cp >= 0x0370 && cp <= 0x058F) ||
           (cp >= 0x0900 && cp <= 0x1FFF) ||
           (cp >= 0x2C00 && cp <= 0xD7FF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF21 && cp <= 0xFF3A) ||
           (cp >= 0xFF41 && cp <= 0xFF5A) ||
           (cp >= 0x10000 && cp <= 0x107FF) ||
           (cp >= 0x11000 && cp <= 0x1E7FF);
}

// Returns 1 for the first strong LTR character, -1 for RTL, 0 if neutral.
int scan_direction(const std::string& text, std::size_t begin, std::size_t end) {
    std::size_t i = begin;
    while (i < end) {
        std::uint32_t cp = 0;
        i = utf8_decode(text, i, end, cp);
        if (is_rtl_codepoint(cp)) return -1;
        if (is_ltr_strong_codepoint(cp)) return 1;
    }
    return 0;
}

bool is_ascii_wrap_space(char c) {
    return c == ' ' || c == '\t';
}

std::size_t trim_trailing_ascii_spaces(const std::string& text, std::size_t begin,
                                       std::size_t end) {
    while (end > begin) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r') {
            --end;
        } else {
            break;
        }
    }
    return end;
}

std::vector<std::string> split_families(const std::string& value) {
    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::size_t stop = comma == std::string::npos ? value.size() : comma;
        std::size_t a = pos;
        std::size_t b = stop;
        while (a < b && (value[a] == ' ' || value[a] == '\t')) ++a;
        while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t')) --b;
        if (b > a) result.emplace_back(value.substr(a, b - a));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

std::string trim_copy(const std::string& value) {
    std::size_t a = 0;
    std::size_t b = value.size();
    while (a < b && (value[a] == ' ' || value[a] == '\t' || value[a] == '\r' ||
                     value[a] == '\n')) {
        ++a;
    }
    while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t' ||
                     value[b - 1] == '\r' || value[b - 1] == '\n')) {
        --b;
    }
    return value.substr(a, b - a);
}

std::vector<hb_feature_t> parse_features(const std::string& value) {
    std::vector<hb_feature_t> result;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::size_t stop = comma == std::string::npos ? value.size() : comma;
        const std::string token = trim_copy(value.substr(pos, stop - pos));
        if (!token.empty()) {
            hb_feature_t feature{};
            if (hb_feature_from_string(token.c_str(), static_cast<int>(token.size()),
                                       &feature)) {
                result.push_back(feature);
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

bool is_whitespace_codepoint(std::uint32_t cp) {
    return cp == 0x0009 || cp == 0x000A || cp == 0x000B || cp == 0x000C ||
           cp == 0x000D || cp == 0x0020 || cp == 0x0085 || cp == 0x00A0 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool is_format_codepoint(std::uint32_t cp) {
    return cp == 0x200C || cp == 0x200D ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

std::string expand_tilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    if (path.size() > 1 && path[1] != '/') return path;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return path;
    if (path.size() == 1) return std::string(home);
    return std::string(home) + path.substr(1);
}

bool file_is_regular(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

std::string canonical_or_original(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return ec ? path : canonical.string();
}

std::string resolve_explicit_path(const std::string& src, const std::string& base_dir,
                                  std::string& error) {
    const std::string expanded = expand_tilde(src);
    const std::filesystem::path raw(expanded);
    std::vector<std::string> candidates;

    auto add_candidate = [&candidates](const std::filesystem::path& p) {
        const std::string value = p.string();
        if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
            candidates.push_back(value);
        }
    };

    if (raw.is_absolute()) {
        add_candidate(raw);
    } else {
        if (!base_dir.empty()) add_candidate(std::filesystem::path(base_dir) / raw);
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec) add_candidate(cwd / raw);
    }

    for (const std::string& candidate : candidates) {
        if (file_is_regular(candidate)) return canonical_or_original(candidate);
    }

    error = "font file not found: '" + src + "'";
    if (!candidates.empty()) {
        error += " (tried:";
        for (const std::string& candidate : candidates) {
            error += " '";
            error += candidate;
            error += "'";
        }
        error += ")";
    }
    return {};
}

// ------------------------------- Faces -------------------------------------

struct FaceEntry {
    FT_Face face = nullptr;
    hb_face_t* hb_face = nullptr;
    hb_font_t* hb_font = nullptr;
    std::string path;
    std::string family;
    std::string style;
    int face_index = 0;
    int upem = 1000;
    double ascent_units = 0.0;
    double descent_units = 0.0;
    std::uint64_t last_use = 0;

    FaceEntry() = default;
    FaceEntry(const FaceEntry&) = delete;
    FaceEntry& operator=(const FaceEntry&) = delete;

    ~FaceEntry() {
        if (hb_font) hb_font_destroy(hb_font);
        if (hb_face) hb_face_destroy(hb_face);
        if (face) FT_Done_Face(face);
    }
};

struct ResolvedFont {
    bool ok = false;
    bool valid_face = false;
    std::string path;
    int face_index = 0;
    std::string family;
    std::string style;
};

// ----------------------------- Outlines ------------------------------------

struct OutlineBuildCtx {
    double scale = 1.0;
    double origin_x = 0.0;
    double origin_y = 0.0;
    double tolerance_sq = kFlattenTolerancePx * kFlattenTolerancePx;
    std::vector<FlattenedSubpath>* output = nullptr;
    FlattenedSubpath current;

    Vec2 map(const FT_Vector& v) const {
        return {origin_x + static_cast<double>(v.x) * scale,
                origin_y - static_cast<double>(v.y) * scale};
    }

    void add_point(const Vec2& p) {
        if (!current.points.empty()) {
            const Vec2 delta = p - current.points.back();
            if (delta.length_sq() <= 1e-12) return;
        }
        current.points.push_back(p);
    }

    void flush() {
        if (current.points.size() >= 2) {
            if ((current.points.back() - current.points.front()).length_sq() <= 1e-12) {
                current.points.pop_back();
            }
            if (current.points.size() >= 2) {
                current.closed = true;
                current.cumulative_lengths.assign(current.points.size(), 0.0);
                double length = 0.0;
                for (std::size_t i = 1; i < current.points.size(); ++i) {
                    length += (current.points[i] - current.points[i - 1]).length();
                    current.cumulative_lengths[i] = length;
                }
                if (output) output->push_back(std::move(current));
            }
        }
        current = FlattenedSubpath{};
    }

    static double point_line_distance_sq(const Vec2& p, const Vec2& a, const Vec2& b) {
        const Vec2 ab = b - a;
        const double denom = ab.length_sq();
        if (denom <= 1e-18) return (p - a).length_sq();
        double t = (p - a).dot(ab) / denom;
        t = std::clamp(t, 0.0, 1.0);
        const Vec2 closest = a + ab * t;
        return (p - closest).length_sq();
    }

    void flatten_quadratic(const Vec2& p0, const Vec2& c, const Vec2& p1,
                           int depth) {
        if (depth >= 18 ||
            point_line_distance_sq(c, p0, p1) <= tolerance_sq) {
            add_point(p1);
            return;
        }
        const Vec2 p01 = (p0 + c) * 0.5;
        const Vec2 p12 = (c + p1) * 0.5;
        const Vec2 mid = (p01 + p12) * 0.5;
        flatten_quadratic(p0, p01, mid, depth + 1);
        flatten_quadratic(mid, p12, p1, depth + 1);
    }

    void flatten_cubic(const Vec2& p0, const Vec2& c1, const Vec2& c2,
                       const Vec2& p1, int depth) {
        const double d1 = point_line_distance_sq(c1, p0, p1);
        const double d2 = point_line_distance_sq(c2, p0, p1);
        if (depth >= 18 || std::max(d1, d2) <= tolerance_sq) {
            add_point(p1);
            return;
        }
        const Vec2 p01 = (p0 + c1) * 0.5;
        const Vec2 p12 = (c1 + c2) * 0.5;
        const Vec2 p23 = (c2 + p1) * 0.5;
        const Vec2 p012 = (p01 + p12) * 0.5;
        const Vec2 p123 = (p12 + p23) * 0.5;
        const Vec2 mid = (p012 + p123) * 0.5;
        flatten_cubic(p0, p01, p012, mid, depth + 1);
        flatten_cubic(mid, p123, p23, p1, depth + 1);
    }

    void move_to(const FT_Vector* to) {
        flush();
        current.points.push_back(map(*to));
    }

    void line_to(const FT_Vector* to) { add_point(map(*to)); }

    void conic_to(const FT_Vector* control, const FT_Vector* to) {
        if (current.points.empty()) {
            move_to(to);
            return;
        }
        const Vec2 p0 = current.points.back();
        flatten_quadratic(p0, map(*control), map(*to), 0);
    }

    void cubic_to(const FT_Vector* control1, const FT_Vector* control2,
                  const FT_Vector* to) {
        if (current.points.empty()) {
            move_to(to);
            return;
        }
        const Vec2 p0 = current.points.back();
        flatten_cubic(p0, map(*control1), map(*control2), map(*to), 0);
    }
};

extern "C" int mesh_outline_move_to(const FT_Vector* to, void* user) {
    static_cast<OutlineBuildCtx*>(user)->move_to(to);
    return 0;
}
extern "C" int mesh_outline_line_to(const FT_Vector* to, void* user) {
    static_cast<OutlineBuildCtx*>(user)->line_to(to);
    return 0;
}
extern "C" int mesh_outline_conic_to(const FT_Vector* control, const FT_Vector* to,
                                     void* user) {
    static_cast<OutlineBuildCtx*>(user)->conic_to(control, to);
    return 0;
}
extern "C" int mesh_outline_cubic_to(const FT_Vector* control1,
                                     const FT_Vector* control2, const FT_Vector* to,
                                     void* user) {
    static_cast<OutlineBuildCtx*>(user)->cubic_to(control1, control2, to);
    return 0;
}

void append_glyph_outline(FT_Face face, std::uint32_t glyph_index, double origin_x,
                          double origin_y, double scale,
                          std::vector<FlattenedSubpath>& output, BBox& bbox) {
    const FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING |
                           FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT;
    if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph_index), flags) != 0) return;
    if (!face->glyph || face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return;

    const std::size_t first_new_subpath = output.size();
    OutlineBuildCtx ctx;
    ctx.scale = scale;
    ctx.origin_x = origin_x;
    ctx.origin_y = origin_y;
    ctx.output = &output;

    FT_Outline_Funcs funcs{};
    funcs.move_to = mesh_outline_move_to;
    funcs.line_to = mesh_outline_line_to;
    funcs.conic_to = mesh_outline_conic_to;
    funcs.cubic_to = mesh_outline_cubic_to;
    funcs.shift = 0;
    funcs.delta = 0;

    if (FT_Outline_Decompose(&face->glyph->outline, &funcs, &ctx) != 0) return;
    ctx.flush();

    for (std::size_t i = first_new_subpath; i < output.size(); ++i) {
        for (const Vec2& point : output[i].points) bbox.include(point);
    }
}

// ------------------------------ Shaping ------------------------------------

struct ShapedGlyph {
    std::uint32_t glyph_index = 0;
    double advance = 0.0;
    double x_offset = 0.0;
    double y_offset = 0.0;
    std::size_t cluster = 0;
    bool space = false;
};

struct ShapedRun {
    std::shared_ptr<FaceEntry> face;
    bool rtl = false;
    double width = 0.0;
    double origin = 0.0;
    std::size_t start = 0;
    std::size_t end = 0;
    std::vector<ShapedGlyph> glyphs;
};

struct LineShape {
    std::vector<ShapedRun> runs;
    double width = 0.0;
    bool rtl = false;
};

struct ShapeOptions {
    std::shared_ptr<FaceEntry> primary;
    const FontRequest* font = nullptr;
    double size = 18.0;
    double letter_spacing = 0.0;
    double word_spacing = 0.0;
    const std::vector<hb_feature_t>* features = nullptr;
    hb_language_t language = HB_LANGUAGE_INVALID;
};

struct ByteRange {
    std::size_t start = 0;
    std::size_t end = 0;
};

bool needs_fallback(std::uint32_t cp) {
    if (cp <= 0x20) return false;
    if (cp >= 0x7F && cp <= 0x9F) return false;
    if (is_whitespace_codepoint(cp)) return false;
    if (is_format_codepoint(cp)) return false;
    return true;
}

#endif // MESH_FONT_CORE

} // namespace

// ------------------------------- Impl --------------------------------------

struct FontBackend::Impl {
#if defined(MESH_FONT_CORE)
    FT_Library ft = nullptr;
    bool ft_ready = false;
    std::string ft_error;
    std::string ft_version;
    std::string hb_version;
    std::string fc_version;

    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<FaceEntry>> faces;
    std::unordered_map<std::string, ResolvedFont> family_cache;
    std::unordered_map<std::string, ResolvedFont> fallback_cache;
    std::uint64_t tick = 0;

    Impl();
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    std::shared_ptr<FaceEntry> load_face(const std::string& path, int face_index,
                                         std::string& error);
    Result resolve_font_locked(const FontRequest& request,
                               const std::string& base_dir, ResolvedFont& out);
    std::shared_ptr<FaceEntry> find_fallback_locked(const FontRequest& request,
                                                    const std::string& primary_family,
                                                    std::uint32_t cp);
    LineShape shape_line_locked(const std::string& text, std::size_t begin,
                                std::size_t end, const ShapeOptions& options,
                                bool paragraph_rtl);
    double measure_locked(const std::string& text, std::size_t begin,
                          std::size_t end, const ShapeOptions& options,
                          bool paragraph_rtl);
    void wrap_locked(const std::string& text, std::size_t begin, std::size_t end,
                     double max_width, const ShapeOptions& options,
                     bool paragraph_rtl, std::vector<ByteRange>& lines);
    void emit_line_locked(const LineShape& line, double baseline,
                          double align_offset, double size,
                          std::vector<FlattenedSubpath>& output, BBox& bbox);
    Result layout_text_locked(const FontRequest& font, const TextRequest& text,
                              const std::string& base_dir, TextLayout& out);
#else
    Impl() = default;
    ~Impl() = default;
#endif
};

#if defined(MESH_FONT_CORE)

namespace {

#if defined(MESH_HAVE_FONTCONFIG)
int fontconfig_weight(double weight) {
    if (!(weight > 0.0) || !std::isfinite(weight)) weight = 400.0;
    weight = std::clamp(weight, 1.0, 1000.0);
#  if defined(FC_VERSION) && FC_VERSION >= 21200
    return static_cast<int>(std::lround(FcWeightFromOpenTypeDouble(weight)));
#  else
    if (weight < 150.0) return FC_WEIGHT_THIN;
    if (weight < 250.0) return FC_WEIGHT_EXTRALIGHT;
    if (weight < 350.0) return FC_WEIGHT_LIGHT;
    if (weight < 450.0) return FC_WEIGHT_REGULAR;
    if (weight < 550.0) return FC_WEIGHT_MEDIUM;
    if (weight < 650.0) return FC_WEIGHT_DEMIBOLD;
    if (weight < 750.0) return FC_WEIGHT_BOLD;
    if (weight < 850.0) return FC_WEIGHT_EXTRABOLD;
    return FC_WEIGHT_BLACK;
#  endif
}

int fontconfig_slant(int slant) {
    if (slant == 1) return FC_SLANT_ITALIC;
    if (slant == 2) return FC_SLANT_OBLIQUE;
    return FC_SLANT_ROMAN;
}

FcPattern* make_fc_pattern(const std::vector<std::string>& families,
                           const FontRequest& request) {
    FcPattern* pattern = FcPatternCreate();
    if (!pattern) return nullptr;
    const std::vector<std::string>& use_families =
        families.empty() ? std::vector<std::string>{"sans-serif"} : families;
    for (const std::string& family : use_families) {
        FcPatternAddString(pattern, FC_FAMILY,
                           reinterpret_cast<const FcChar8*>(family.c_str()));
    }
    FcPatternAddInteger(pattern, FC_WEIGHT, fontconfig_weight(request.weight));
    FcPatternAddInteger(pattern, FC_SLANT, fontconfig_slant(request.slant));
    double stretch = request.stretch;
    if (!(stretch > 0.0) || !std::isfinite(stretch)) stretch = 100.0;
    FcPatternAddDouble(pattern, FC_WIDTH, stretch);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    return pattern;
}

bool extract_fc_pattern(FcPattern* pattern, ResolvedFont& out) {
    if (!pattern) return false;
    FcChar8* file = nullptr;
    if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch || !file) {
        return false;
    }
    int index = 0;
    if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch) index = 0;

    out.valid_face = true;
    out.path = reinterpret_cast<const char*>(file);
    out.face_index = index;

    FcChar8* family = nullptr;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch && family) {
        out.family = reinterpret_cast<const char*>(family);
    }
    FcChar8* style = nullptr;
    if (FcPatternGetString(pattern, FC_STYLE, 0, &style) == FcResultMatch && style) {
        out.style = reinterpret_cast<const char*>(style);
    }
    return !out.path.empty();
}

bool fc_pattern_covers(FcPattern* pattern, std::uint32_t cp) {
    FcCharSet* charset = nullptr;
    if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &charset) != FcResultMatch ||
        !charset) {
        return false;
    }
    return FcCharSetHasChar(charset, cp) != 0;
}

FcPattern* fc_match(const std::vector<std::string>& families,
                    const FontRequest& request) {
    FcPattern* pattern = make_fc_pattern(families, request);
    if (!pattern) return nullptr;
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    return match;
}
#endif // MESH_HAVE_FONTCONFIG

#if defined(MESH_HAVE_FONTCONFIG)
std::string style_cache_key(const FontRequest& request) {
    double weight = request.weight;
    if (!(weight > 0.0) || !std::isfinite(weight)) weight = 400.0;
    double stretch = request.stretch;
    if (!(stretch > 0.0) || !std::isfinite(stretch)) stretch = 100.0;
    return std::to_string(static_cast<int>(std::lround(weight))) + "|" +
           std::to_string(request.slant) + "|" +
           std::to_string(static_cast<int>(std::lround(stretch)));
}
#endif

} // namespace

FontBackend::Impl::Impl() {
    hb_version = HB_VERSION_STRING;
    FT_Error error = FT_Init_FreeType(&ft);
    if (error != 0) {
        ft = nullptr;
        ft_ready = false;
        ft_error = "FreeType initialization failed (error " +
                   std::to_string(static_cast<int>(error)) + ")";
    } else {
        ft_ready = true;
        FT_Int major = 0;
        FT_Int minor = 0;
        FT_Int patch = 0;
        FT_Library_Version(ft, &major, &minor, &patch);
        ft_version = std::to_string(major) + "." + std::to_string(minor) + "." +
                     std::to_string(patch);
    }

#if defined(MESH_HAVE_FONTCONFIG)
    const int version = FcGetVersion();
    fc_version = std::to_string(version / 10000) + "." +
                 std::to_string((version / 100) % 100) + "." +
                 std::to_string(version % 100);
#else
    fc_version = "unavailable (built without Fontconfig)";
#endif
}

FontBackend::Impl::~Impl() {
    faces.clear();
    family_cache.clear();
    fallback_cache.clear();
    if (ft) {
        FT_Done_FreeType(ft);
        ft = nullptr;
    }
}

std::shared_ptr<FaceEntry> FontBackend::Impl::load_face(const std::string& path,
                                                        int face_index,
                                                        std::string& error) {
    if (!ft_ready || !ft) {
        error = "FreeType is not available";
        return nullptr;
    }
    if (face_index < 0) face_index = 0;

    const std::string key = path + "#" + std::to_string(face_index);
    auto cached = faces.find(key);
    if (cached != faces.end()) {
        cached->second->last_use = ++tick;
        return cached->second;
    }

    auto entry = std::make_shared<FaceEntry>();
    FT_Error ft_error = FT_New_Face(ft, path.c_str(), face_index, &entry->face);
    if (ft_error != 0 || !entry->face) {
        error = "FreeType could not open '" + path + "' at face index " +
                std::to_string(face_index) + " (error " +
                std::to_string(static_cast<int>(ft_error)) + ")";
        return nullptr;
    }
    if (face_index >= entry->face->num_faces) {
        error = "face index " + std::to_string(face_index) +
                " is out of range for '" + path + "' (" +
                std::to_string(entry->face->num_faces) + " faces)";
        return nullptr;
    }

    FT_Select_Charmap(entry->face, FT_ENCODING_UNICODE);

    entry->path = path;
    entry->face_index = face_index;
    entry->upem = entry->face->units_per_EM > 0 ? entry->face->units_per_EM : 1000;
    entry->ascent_units = static_cast<double>(entry->face->ascender);
    entry->descent_units = -static_cast<double>(entry->face->descender);
    if (!(entry->ascent_units > 0.0)) {
        entry->ascent_units = static_cast<double>(entry->upem) * 0.8;
    }
    if (!(entry->descent_units > 0.0)) {
        entry->descent_units = static_cast<double>(entry->upem) * 0.2;
    }
    if (entry->face->family_name) entry->family = entry->face->family_name;
    if (entry->face->style_name) entry->style = entry->face->style_name;

    hb_blob_t* blob = hb_blob_create_from_file(path.c_str());
    if (!blob) {
        error = "HarfBuzz could not open '" + path + "'";
        return nullptr;
    }
    unsigned int blob_length = 0;
    (void)hb_blob_get_data(blob, &blob_length);
    if (blob_length == 0) {
        hb_blob_destroy(blob);
        error = "font file is empty: '" + path + "'";
        return nullptr;
    }
    entry->hb_face = hb_face_create(blob, static_cast<unsigned int>(face_index));
    hb_blob_destroy(blob);
    if (!entry->hb_face || hb_face_get_glyph_count(entry->hb_face) == 0) {
        error = "HarfBuzz could not read font tables from '" + path + "'";
        return nullptr;
    }
    entry->hb_font = hb_font_create(entry->hb_face);
    if (!entry->hb_font) {
        error = "HarfBuzz could not create a font object for '" + path + "'";
        return nullptr;
    }
    hb_ot_font_set_funcs(entry->hb_font);
    hb_font_set_scale(entry->hb_font, entry->upem, entry->upem);
    entry->last_use = ++tick;

    faces.emplace(key, entry);
    if (faces.size() > 128) {
        auto victim = faces.end();
        for (auto it = faces.begin(); it != faces.end(); ++it) {
            if (it->first == key) continue;
            if (victim == faces.end() ||
                it->second->last_use < victim->second->last_use) {
                victim = it;
            }
        }
        if (victim != faces.end()) faces.erase(victim);
    }
    return entry;
}

Result FontBackend::Impl::resolve_font_locked(const FontRequest& request,
                                              const std::string& base_dir,
                                              ResolvedFont& out) {
    out = ResolvedFont{};
    if (!ft_ready) {
        Result result;
        result.ok = false;
        result.error = ft_error.empty() ? "FreeType is not available" : ft_error;
        return result;
    }

    if (!request.src.empty()) {
        std::string path_error;
        const std::string path = resolve_explicit_path(request.src, base_dir, path_error);
        if (path.empty()) {
            Result result;
            result.ok = false;
            result.error = path_error;
            return result;
        }
        std::string load_error;
        std::shared_ptr<FaceEntry> face =
            load_face(path, request.face_index, load_error);
        if (!face) {
            Result result;
            result.ok = false;
            result.error = load_error;
            return result;
        }
        out.ok = true;
        out.valid_face = true;
        out.path = face->path;
        out.face_index = face->face_index;
        out.family = face->family;
        out.style = face->style;
        return Result{};
    }

    std::vector<std::string> families = split_families(request.family);
    if (families.empty()) families.emplace_back("sans-serif");

#if !defined(MESH_HAVE_FONTCONFIG)
    Result result;
    result.ok = false;
    result.error =
        "font family lookup is unavailable: Mesh was built without Fontconfig; "
        "specify FontRequest::src with an explicit TTF/OTF/TTC path";
    return result;
#else
    std::string key;
    key.reserve(request.family.size() + 48);
    key += request.family;
    key += '|';
    key += style_cache_key(request);
    const auto cached = family_cache.find(key);
    if (cached != family_cache.end()) {
        out = cached->second;
        return Result{};
    }

    FcPattern* match = fc_match(families, request);
    if (!match) {
        Result result;
        result.ok = false;
        result.error = "Fontconfig could not resolve font family '" +
                       request.family + "'";
        return result;
    }
    ResolvedFont resolved;
    const bool extracted = extract_fc_pattern(match, resolved);
    FcPatternDestroy(match);
    if (!extracted) {
        Result result;
        result.ok = false;
        result.error = "Fontconfig returned no usable file for font family '" +
                       request.family + "'";
        return result;
    }

    std::string load_error;
    std::shared_ptr<FaceEntry> face =
        load_face(resolved.path, resolved.face_index, load_error);
    if (!face) {
        Result result;
        result.ok = false;
        result.error = load_error;
        return result;
    }
    resolved.ok = true;
    resolved.valid_face = true;
    resolved.path = face->path;
    resolved.face_index = face->face_index;
    if (!face->family.empty()) resolved.family = face->family;
    if (!face->style.empty()) resolved.style = face->style;

    family_cache[key] = resolved;
    if (family_cache.size() > 512) family_cache.clear();
    out = resolved;
    return Result{};
#endif
}

std::shared_ptr<FaceEntry> FontBackend::Impl::find_fallback_locked(
    const FontRequest& request, const std::string& primary_family,
    std::uint32_t cp) {
#if !defined(MESH_HAVE_FONTCONFIG)
    (void)request;
    (void)primary_family;
    (void)cp;
    return nullptr;
#else
    const std::string key = style_cache_key(request) + "|" + request.fallback + "|" +
                            primary_family + "|" + std::to_string(cp);
    const auto cached = fallback_cache.find(key);
    if (cached != fallback_cache.end()) {
        if (!cached->second.ok) return nullptr;
        std::string load_error;
        return load_face(cached->second.path, cached->second.face_index, load_error);
    }

    std::vector<std::string> families = split_families(request.fallback);
    if (!primary_family.empty()) {
        families.push_back(primary_family);
    } else {
        families.emplace_back("sans-serif");
    }

    auto accept = [&](FcPattern* pattern) -> std::shared_ptr<FaceEntry> {
        ResolvedFont resolved;
        if (!extract_fc_pattern(pattern, resolved)) return nullptr;
        std::string load_error;
        std::shared_ptr<FaceEntry> face =
            load_face(resolved.path, resolved.face_index, load_error);
        if (!face) return nullptr;
        if (FT_Get_Char_Index(face->face, cp) == 0) return nullptr;
        resolved.ok = true;
        fallback_cache[key] = resolved;
        if (fallback_cache.size() > 4096) fallback_cache.clear();
        return face;
    };

    // Prefer requested fallback families, then the primary family.
    for (const std::string& family : families) {
        FcPattern* match = fc_match({family}, request);
        if (!match) continue;
        const bool covers = fc_pattern_covers(match, cp);
        if (covers) {
            if (std::shared_ptr<FaceEntry> face = accept(match)) {
                FcPatternDestroy(match);
                return face;
            }
        }
        FcPatternDestroy(match);
    }

    // Finally ask Fontconfig for its complete, coverage-sorted list.
    FcPattern* pattern = make_fc_pattern(families, request);
    if (pattern) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult sort_result = FcResultNoMatch;
        FcFontSet* set =
            FcFontSort(nullptr, pattern, FcTrue, nullptr, &sort_result);
        if (set) {
            for (int i = 0; i < set->nfont; ++i) {
                FcPattern* candidate = set->fonts[i];
                if (!fc_pattern_covers(candidate, cp)) continue;
                FcPattern* prepared = FcFontRenderPrepare(nullptr, pattern, candidate);
                if (prepared) {
                    if (std::shared_ptr<FaceEntry> face = accept(prepared)) {
                        FcPatternDestroy(prepared);
                        FcFontSetDestroy(set);
                        FcPatternDestroy(pattern);
                        return face;
                    }
                    FcPatternDestroy(prepared);
                }
            }
            FcFontSetDestroy(set);
        }
        FcPatternDestroy(pattern);
    }

    ResolvedFont none;
    none.ok = false;
    fallback_cache[key] = none;
    if (fallback_cache.size() > 4096) fallback_cache.clear();
    return nullptr;
#endif
}

LineShape FontBackend::Impl::shape_line_locked(const std::string& text,
                                               std::size_t begin,
                                               std::size_t end,
                                               const ShapeOptions& options,
                                               bool paragraph_rtl) {
    LineShape line;
    line.rtl = paragraph_rtl;
    if (!options.primary || begin >= end) return line;

    struct PendingRun {
        std::shared_ptr<FaceEntry> face;
        bool rtl = false;
        std::size_t start = 0;
        std::size_t end = 0;
    };
    std::vector<PendingRun> pending;

    bool current_rtl = paragraph_rtl;
    std::shared_ptr<FaceEntry> previous_face;
    std::size_t i = begin;
    while (i < end) {
        std::uint32_t cp = 0;
        const std::size_t next = utf8_decode(text, i, end, cp);

        if (is_rtl_codepoint(cp)) {
            current_rtl = true;
        } else if (is_ltr_strong_codepoint(cp)) {
            current_rtl = false;
        }

        std::shared_ptr<FaceEntry> face = options.primary;
        if (is_format_codepoint(cp) && previous_face) {
            // Keep variation selectors and joiners with the face they modify.
            face = previous_face;
        } else if (needs_fallback(cp) &&
                   FT_Get_Char_Index(options.primary->face, cp) == 0) {
            face = find_fallback_locked(*options.font, options.primary->family, cp);
        }
        if (!face || !face->face ||
            (FT_Get_Char_Index(face->face, cp) == 0 && needs_fallback(cp))) {
            // No fallback covers this codepoint: keep it with the primary run
            // so HarfBuzz emits .notdef and preserves the advance.
            face = options.primary;
        }

        const bool split = pending.empty() || pending.back().face.get() != face.get() ||
                           pending.back().rtl != current_rtl;
        if (split) {
            pending.push_back(PendingRun{face, current_rtl, i, next});
        } else {
            pending.back().end = next;
        }
        previous_face = face;
        i = next;
    }

    for (PendingRun& run : pending) {
        ShapedRun shaped;
        shaped.face = run.face;
        shaped.start = run.start;
        shaped.end = run.end;
        const double scale =
            options.size / static_cast<double>(run.face->upem > 0 ? run.face->upem
                                                                  : 1000);
        shaped.rtl = run.rtl;

        hb_buffer_t* buffer = hb_buffer_create();
        if (!buffer) continue;
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                           static_cast<int>(run.start),
                           static_cast<int>(run.end - run.start));
        hb_buffer_set_direction(buffer, shaped.rtl ? HB_DIRECTION_RTL
                                                   : HB_DIRECTION_LTR);
        if (options.language != HB_LANGUAGE_INVALID) {
            hb_buffer_set_language(buffer, options.language);
        }
        hb_buffer_guess_segment_properties(buffer);

        const hb_feature_t* features =
            options.features && !options.features->empty()
                ? options.features->data()
                : nullptr;
        const unsigned int feature_count =
            options.features && !options.features->empty()
                ? static_cast<unsigned int>(options.features->size())
                : 0u;
        hb_shape(shaped.face->hb_font, buffer, features, feature_count);

        unsigned int glyph_count = 0;
        const hb_glyph_info_t* infos =
            hb_buffer_get_glyph_infos(buffer, &glyph_count);
        const hb_glyph_position_t* positions =
            hb_buffer_get_glyph_positions(buffer, &glyph_count);

        shaped.glyphs.reserve(glyph_count);
        for (unsigned int g = 0; g < glyph_count; ++g) {
            ShapedGlyph glyph;
            glyph.glyph_index = infos[g].codepoint;
            glyph.cluster = infos[g].cluster;
            glyph.advance =
                static_cast<double>(positions[g].x_advance) * scale;
            glyph.x_offset =
                static_cast<double>(positions[g].x_offset) * scale;
            glyph.y_offset =
                static_cast<double>(positions[g].y_offset) * scale;
            if (glyph.cluster < text.size()) {
                std::uint32_t cp = 0;
                (void)utf8_decode(text, glyph.cluster, text.size(), cp);
                glyph.space = cp == 0x20;
            }
            // Spacing belongs to grapheme clusters, not to every glyph of a
            // base + combining-mark (or ligature) cluster.
            const bool last_in_cluster =
                (g + 1 == glyph_count) || infos[g + 1].cluster != infos[g].cluster;
            if (last_in_cluster) {
                glyph.advance += options.letter_spacing;
                if (glyph.space) glyph.advance += options.word_spacing;
            }
            if (glyph.advance < 0.0) glyph.advance = 0.0;
            shaped.width += glyph.advance;
            shaped.glyphs.push_back(glyph);
        }
        hb_buffer_destroy(buffer);
        line.runs.push_back(std::move(shaped));
    }

    double total = 0.0;
    for (const ShapedRun& run : line.runs) total += run.width;

    double prefix = 0.0;
    for (ShapedRun& run : line.runs) {
        if (paragraph_rtl) {
            run.origin = total - prefix - run.width;
        } else {
            run.origin = prefix;
        }
        prefix += run.width;
    }
    line.width = total;
    return line;
}

double FontBackend::Impl::measure_locked(const std::string& text,
                                         std::size_t begin, std::size_t end,
                                         const ShapeOptions& options,
                                         bool paragraph_rtl) {
    if (begin >= end) return 0.0;
    return shape_line_locked(text, begin, end, options, paragraph_rtl).width;
}

void FontBackend::Impl::wrap_locked(const std::string& text, std::size_t begin,
                                    std::size_t end, double max_width,
                                    const ShapeOptions& options,
                                    bool paragraph_rtl,
                                    std::vector<ByteRange>& lines) {
    if (begin >= end) {
        lines.push_back(ByteRange{begin, begin});
        return;
    }
    if (!(max_width > 0.0)) {
        lines.push_back(ByteRange{begin, end});
        return;
    }

    std::size_t line_start = begin;
    std::size_t pos = begin;

    auto break_long_word = [&](std::size_t word_start, std::size_t word_end) {
        std::size_t segment_start = word_start;
        std::size_t p = word_start;
        while (p < word_end) {
            std::uint32_t cp = 0;
            const std::size_t next = utf8_decode(text, p, word_end, cp);
            const double width =
                measure_locked(text, segment_start, next, options, paragraph_rtl);
            if (width > max_width && next > segment_start && p > segment_start) {
                lines.push_back(ByteRange{segment_start, p});
                segment_start = p;
                continue;
            }
            p = next;
        }
        if (segment_start < word_end) {
            lines.push_back(ByteRange{segment_start, word_end});
        }
    };

    while (pos < end) {
        const std::size_t word_start = pos;
        while (pos < end && !is_ascii_wrap_space(text[pos])) {
            std::uint32_t cp = 0;
            pos = utf8_decode(text, pos, end, cp);
        }
        const std::size_t word_end = pos;
        while (pos < end && is_ascii_wrap_space(text[pos])) {
            ++pos;
        }
        const std::size_t token_end = pos;

        if (word_end == word_start) {
            continue;
        }

        const double candidate_width =
            measure_locked(text, line_start, token_end, options, paragraph_rtl);
        const double word_width =
            measure_locked(text, word_start, word_end, options, paragraph_rtl);

        if (candidate_width <= max_width || line_start == word_start) {
            if (line_start == word_start && word_width > max_width) {
                break_long_word(word_start, word_end);
                line_start = token_end;
            }
            // Otherwise the whole token fits and remains on this line.
        } else {
            const std::size_t break_end =
                trim_trailing_ascii_spaces(text, line_start, word_start);
            if (break_end > line_start) {
                lines.push_back(ByteRange{line_start, break_end});
            }
            line_start = word_start;
            pos = word_start;
            continue;
        }
    }

    if (line_start < end) {
        const std::size_t break_end = trim_trailing_ascii_spaces(text, line_start, end);
        if (break_end > line_start) {
            lines.push_back(ByteRange{line_start, break_end});
        } else if (lines.empty()) {
            lines.push_back(ByteRange{line_start, line_start});
        }
    } else if (lines.empty()) {
        lines.push_back(ByteRange{begin, begin});
    }
}

void FontBackend::Impl::emit_line_locked(const LineShape& line, double baseline,
                                         double align_offset, double size,
                                         std::vector<FlattenedSubpath>& output,
                                         BBox& bbox) {
    for (const ShapedRun& run : line.runs) {
        if (!run.face || !run.face->face) continue;
        const double scale = size / static_cast<double>(run.face->upem);
        double pen = align_offset + run.origin;
        for (const ShapedGlyph& glyph : run.glyphs) {
            const double glyph_x = pen + glyph.x_offset;
            const double glyph_y = baseline - glyph.y_offset;
            append_glyph_outline(run.face->face, glyph.glyph_index, glyph_x,
                                 glyph_y, scale, output, bbox);
            pen += glyph.advance;
        }
    }
}

Result FontBackend::Impl::layout_text_locked(const FontRequest& font,
                                             const TextRequest& text,
                                             const std::string& base_dir,
                                             TextLayout& out) {
    out = TextLayout{};

    if (!(text.size > 0.0) || !std::isfinite(text.size)) {
        Result result;
        result.ok = false;
        result.error = "text size must be a positive finite number";
        return result;
    }
    if (text.text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        Result result;
        result.ok = false;
        result.error = "text is too large to shape";
        return result;
    }

    ResolvedFont resolved;
    Result resolution = resolve_font_locked(font, base_dir, resolved);
    if (!resolution.ok) return resolution;

    std::string load_error;
    std::shared_ptr<FaceEntry> primary =
        load_face(resolved.path, resolved.face_index, load_error);
    if (!primary) {
        Result result;
        result.ok = false;
        result.error = load_error;
        return result;
    }

    const double size = text.size;
    out.resolved_family = primary->family.empty() ? resolved.family : primary->family;
    out.resolved_path = primary->path;

    const double units_to_px = size / static_cast<double>(primary->upem);
    out.ascent = primary->ascent_units * units_to_px;
    out.descent = primary->descent_units * units_to_px;

    double line_height_multiplier = text.line_height;
    if (!(line_height_multiplier > 0.0) || !std::isfinite(line_height_multiplier)) {
        line_height_multiplier = 1.25;
    }
    const double line_step = size * line_height_multiplier;
    out.line_height = line_step;

    if (text.text.empty()) {
        out.line_count = 0;
        return Result{};
    }

    ShapeOptions options;
    options.primary = primary;
    options.font = &font;
    options.size = size;
    options.letter_spacing = text.letter_spacing;
    options.word_spacing = text.word_spacing;

    const std::vector<hb_feature_t> features = parse_features(text.features);
    options.features = &features;
    if (!text.language.empty()) {
        options.language = hb_language_from_string(text.language.c_str(), -1);
    }

    int document_direction = scan_direction(text.text, 0, text.text.size());
    if (document_direction == 0) document_direction = 1;
    const bool document_rtl = document_direction < 0;

    struct Paragraph {
        std::size_t start = 0;
        std::size_t end = 0;
        bool rtl = false;
    };
    std::vector<Paragraph> paragraphs;
    {
        std::size_t paragraph_start = 0;
        for (std::size_t i = 0; i <= text.text.size(); ++i) {
            if (i != text.text.size() && text.text[i] != '\n') continue;
            std::size_t paragraph_end = i;
            if (paragraph_end > paragraph_start &&
                text.text[paragraph_end - 1] == '\r') {
                --paragraph_end;
            }
            int direction =
                scan_direction(text.text, paragraph_start, paragraph_end);
            if (direction == 0) direction = document_direction;
            paragraphs.push_back(
                Paragraph{paragraph_start, paragraph_end, direction < 0});
            paragraph_start = i + 1;
            if (i == text.text.size()) break;
        }
    }

    std::vector<ByteRange> line_ranges;
    for (const Paragraph& paragraph : paragraphs) {
        if (text.wrap && text.max_width > 0.0) {
            wrap_locked(text.text, paragraph.start, paragraph.end, text.max_width,
                        options, paragraph.rtl, line_ranges);
        } else {
            line_ranges.push_back(ByteRange{paragraph.start, paragraph.end});
        }
    }
    if (line_ranges.empty()) line_ranges.push_back(ByteRange{0, 0});

    out.lines.reserve(line_ranges.size());
    int line_index = 0;
    for (const ByteRange& range : line_ranges) {
        const bool paragraph_rtl =
            [&]() {
                for (const Paragraph& paragraph : paragraphs) {
                    if (range.start >= paragraph.start &&
                        range.start <= paragraph.end) {
                        return paragraph.rtl;
                    }
                }
                return document_rtl;
            }();

        LineShape shape = shape_line_locked(text.text, range.start, range.end,
                                            options, paragraph_rtl);
        double align_offset = 0.0;
        if (text.align == 1) {
            align_offset = -shape.width * 0.5;
        } else if (text.align == 2) {
            align_offset = -shape.width;
        }
        const double baseline = static_cast<double>(line_index) * line_step;

        TextLineMetrics metrics;
        metrics.width = shape.width;
        metrics.baseline = baseline;
        metrics.start = range.start;
        metrics.end = range.end;
        out.lines.push_back(metrics);
        out.advance_width = std::max(out.advance_width, shape.width);

        emit_line_locked(shape, baseline, align_offset, size, out.subpaths,
                         out.ink_bbox);
        ++line_index;
    }

    out.line_count = static_cast<int>(out.lines.size());
    return Result{};
}

#endif // MESH_FONT_CORE

// --------------------------- Public facade ---------------------------------

FontBackend& FontBackend::instance() {
    static FontBackend backend;
    return backend;
}

FontBackend::FontBackend() {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_.reset();
    }
}

FontBackend::~FontBackend() = default;

FontBackendStatus FontBackend::status() const {
    FontBackendStatus status;
#if defined(MESH_FONT_CORE)
    if (!impl_) {
        status.available = false;
        status.message = "font backend could not be initialized (out of memory)";
        return status;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    status.available = impl_->ft_ready;
    status.free_type_version =
        impl_->ft_version.empty() ? std::string("unavailable") : impl_->ft_version;
    status.harfbuzz_version = impl_->hb_version;
    status.fontconfig_version = impl_->fc_version;
    if (impl_->ft_ready) {
        status.message = "FreeType and HarfBuzz text backend ready";
#if !defined(MESH_HAVE_FONTCONFIG)
        status.message += "; Fontconfig unavailable, family lookup disabled";
#endif
    } else {
        status.message = impl_->ft_error.empty()
                             ? "FreeType is unavailable"
                             : impl_->ft_error;
    }
#else
    status.available = false;
    status.free_type_version = "unavailable";
    status.harfbuzz_version = "unavailable";
    status.fontconfig_version = "unavailable";
    status.message =
        "font outline backend unavailable: Mesh was built without FreeType "
        "and/or HarfBuzz";
#endif
    return status;
}

Result FontBackend::layout_text(const FontRequest& font, const TextRequest& text,
                                const std::string& base_dir, TextLayout& out) {
    out = TextLayout{};
#if defined(MESH_FONT_CORE)
    if (!impl_) {
        Result result;
        result.ok = false;
        result.error = "font backend could not be initialized (out of memory)";
        return result;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        return impl_->layout_text_locked(font, text, base_dir, out);
    } catch (const std::bad_alloc&) {
        out = TextLayout{};
        Result result;
        result.ok = false;
        result.error = "out of memory while laying out text";
        return result;
    } catch (const std::exception& e) {
        out = TextLayout{};
        Result result;
        result.ok = false;
        result.error = std::string("text layout failed: ") + e.what();
        return result;
    } catch (...) {
        out = TextLayout{};
        Result result;
        result.ok = false;
        result.error = "text layout failed with an unknown error";
        return result;
    }
#else
    (void)font;
    (void)text;
    (void)base_dir;
    Result result;
    result.ok = false;
    result.error =
        "font outline backend unavailable: Mesh was built without FreeType "
        "and/or HarfBuzz";
    return result;
#endif
}

Result FontBackend::resolve_font(const FontRequest& font,
                                 const std::string& base_dir,
                                 std::string& out_path, int& out_face_index,
                                 std::string& out_family, std::string& out_style) {
    out_path.clear();
    out_face_index = 0;
    out_family.clear();
    out_style.clear();
#if defined(MESH_FONT_CORE)
    if (!impl_) {
        Result result;
        result.ok = false;
        result.error = "font backend could not be initialized (out of memory)";
        return result;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        ResolvedFont resolved;
        Result result = impl_->resolve_font_locked(font, base_dir, resolved);
        if (!result.ok) return result;
        out_path = resolved.path;
        out_face_index = resolved.face_index;
        out_family = resolved.family;
        out_style = resolved.style;
        return Result{};
    } catch (const std::bad_alloc&) {
        Result result;
        result.ok = false;
        result.error = "out of memory while resolving font";
        return result;
    } catch (const std::exception& e) {
        Result result;
        result.ok = false;
        result.error = std::string("font resolution failed: ") + e.what();
        return result;
    } catch (...) {
        Result result;
        result.ok = false;
        result.error = "font resolution failed with an unknown error";
        return result;
    }
#else
    (void)font;
    (void)base_dir;
    Result result;
    result.ok = false;
    result.error =
        "font outline backend unavailable: Mesh was built without FreeType "
        "and/or HarfBuzz";
    return result;
#endif
}

} // namespace mesh
