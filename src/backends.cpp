// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "../include/mesh.hpp"
#include "../include/mesh_c_api.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>

namespace mesh {

// ============================================================================
// 1. Base64 & CRC32 Helpers
// ============================================================================

static uint32_t crc32_buf(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
    }
    return ~crc;
}

static std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        if (i + 1 < data.size()) {
            out.push_back(tbl[(n >> 6) & 63]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

// ============================================================================
// 2. Binary Wire Format (`.meshb` v4 — Magic `MSHB` + CRC32) & Full Decompiler
// ============================================================================

struct BinWriter {
    std::vector<uint8_t>& buf;
    uint16_t version = 5;
    explicit BinWriter(std::vector<uint8_t>& b) : buf(b) {}

    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
    void f64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void vec2(const Vec2& v) { f64(v.x); f64(v.y); }
    void color(const Color& c) { f64(c.r); f64(c.g); f64(c.b); f64(c.a); }
    void paint(const PaintServer& ps) {
        u8(static_cast<uint8_t>(ps.kind));
        str(ps.id);
        color(ps.solid_color);
        vec2(ps.start);
        vec2(ps.end);
        vec2(ps.center);
        f64(ps.radius);
        f64(ps.angle_deg);
        u8(ps.user_space ? 1 : 0);
        u32(static_cast<uint32_t>(ps.stops.size()));
        for (const auto& st : ps.stops) {
            f64(st.offset);
            color(st.color);
        }
        u32(static_cast<uint32_t>(ps.rows));
        u32(static_cast<uint32_t>(ps.cols));
        u32(static_cast<uint32_t>(ps.grid_points.size()));
        for (const auto& gp : ps.grid_points) {
            f64(gp.u); f64(gp.v); color(gp.color); f64(gp.weight);
        }
        f64(ps.falloff_exponent);
        u32(static_cast<uint32_t>(ps.diffusion_points.size()));
        for (const auto& dp : ps.diffusion_points) {
            f64(dp.u); f64(dp.v); color(dp.color); f64(dp.weight);
        }
        f64(ps.noise_scale);
        u32(static_cast<uint32_t>(ps.noise_octaves));
        f64(ps.noise_persistence);
        f64(ps.noise_lacunarity);
        f64(ps.noise_seed);
        f64(ps.grain);
    }
    void font_spec(const FontSpec& f) {
        str(f.id);
        str(f.family);
        str(f.src);
        str(f.fallback);
        f64(f.weight);
        u8(static_cast<uint8_t>(f.style));
        f64(f.stretch);
        u32(static_cast<uint32_t>(f.face_index));
    }
    void subpath(const FlattenedSubpath& sp) {
        u8(sp.closed ? 1 : 0);
        u32(static_cast<uint32_t>(sp.points.size()));
        for (const auto& pt : sp.points) vec2(pt);
        u32(static_cast<uint32_t>(sp.cumulative_lengths.size()));
        for (double len : sp.cumulative_lengths) f64(len);
    }
    void node(const ShapeNode& n) {
        u8(static_cast<uint8_t>(n.kind));
        str(n.id);
        paint(n.style.fill);
        paint(n.style.stroke);
        f64(n.style.stroke_width);
        u8(static_cast<uint8_t>(n.style.stroke_align));
        u8(static_cast<uint8_t>(n.style.line_cap));
        u8(static_cast<uint8_t>(n.style.line_join));
        u32(static_cast<uint32_t>(n.style.dash_array.size()));
        for (double dv : n.style.dash_array) f64(dv);
        f64(n.style.dash_offset);
        f64(n.style.opacity);
        f64(n.style.grain);
        u8(static_cast<uint8_t>(n.style.arrow_mode));
        f64(n.style.arrow_size);
        u8(static_cast<uint8_t>(n.style.blend_mode));
        str(n.style.clip_id);
        u8(n.style.shadow.enabled ? 1 : 0);
        f64(n.style.shadow.dx); f64(n.style.shadow.dy); f64(n.style.shadow.blur);
        color(n.style.shadow.color);
        u8(n.style.glow.enabled ? 1 : 0);
        f64(n.style.glow.radius);
        color(n.style.glow.color);

        f64(n.transform.a); f64(n.transform.b); f64(n.transform.c);
        f64(n.transform.d); f64(n.transform.tx); f64(n.transform.ty);

        f64(n.x); f64(n.y); f64(n.width); f64(n.height); f64(n.corner_radius);
        for (int i = 0; i < 4; ++i) f64(n.corner_radii[i]);
        vec2(n.center); f64(n.radius); f64(n.rx); f64(n.ry);
        vec2(n.from); vec2(n.to);
        u32(static_cast<uint32_t>(n.points.size()));
        for (const auto& pt : n.points) vec2(pt);
        f64(n.tension);
        u8(n.closed ? 1 : 0);

        u32(static_cast<uint32_t>(n.path_segments.size()));
        for (const auto& seg : n.path_segments) {
            u8(static_cast<uint8_t>(seg.verb));
            vec2(seg.p1); vec2(seg.p2); vec2(seg.p3);
            f64(seg.radius); f64(seg.bulge);
            u8(seg.clockwise ? 1 : 0);
            u8(seg.large_arc ? 1 : 0);
        }

        u8(static_cast<uint8_t>(n.bool_op));
        f64(n.smoothness);
        u32(static_cast<uint32_t>(n.children.size()));
        for (const auto& ch : n.children) node(ch);

        vec2(n.text_pos);
        str(n.text_content);
        f64(n.font_size);
        f64(n.font_weight);
        u8(static_cast<uint8_t>(n.text_align));
        f64(n.letter_spacing);

        if (version >= 5) {
            f64(n.word_spacing);
            str(n.font_id);
            str(n.font_family);
            str(n.font_src);
            str(n.font_fallback);
            str(n.font_features);
            str(n.font_language);
            u8(static_cast<uint8_t>(n.font_style));
            f64(n.font_stretch);
            u8(n.font_weight_explicit ? 1 : 0);
            u8(n.font_style_explicit ? 1 : 0);
            u8(n.font_stretch_explicit ? 1 : 0);
            f64(n.line_height);
            f64(n.max_width);
            u8(n.wrap ? 1 : 0);
            u8(n.baseline_anchor ? 1 : 0);
            str(n.resolved_font_family);
            str(n.resolved_font_path);
            f64(n.text_width);
            f64(n.text_ascent);
            f64(n.text_descent);
            f64(n.text_line_height);
            u32(static_cast<uint32_t>(std::max(0, n.text_line_count)));
            u32(static_cast<uint32_t>(n.text_subpaths.size()));
            for (const auto& sp : n.text_subpaths) subpath(sp);
        }
    }
};

struct BinReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint16_t version = 4;

    BinReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    void require(size_t n) {
        if (n > size - pos) throw std::runtime_error("Truncated .meshb binary stream");
    }
    void check_count(uint32_t n, size_t min_each, const char* what) {
        if (n != 0 && min_each != 0 && static_cast<size_t>(n) > (size - pos) / min_each) {
            throw std::runtime_error(std::string("Corrupt .meshb: ") + what + " count exceeds payload size");
        }
    }
    uint8_t u8() { require(1); return data[pos++]; }
    uint16_t u16() {
        require(2);
        uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        require(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (static_cast<uint32_t>(data[pos + i]) << (i * 8));
        pos += 4;
        return v;
    }
    double f64() {
        require(8);
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= (static_cast<uint64_t>(data[pos + i]) << (i * 8));
        pos += 8;
        double v = 0.0;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    std::string str() {
        uint32_t len = u32();
        if (len > 128u * 1024u * 1024u) {
            throw std::runtime_error("Corrupt .meshb: string length exceeds 128 MiB cap");
        }
        require(len);
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }
    Vec2 vec2() { double x = f64(); double y = f64(); return {x, y}; }
    Color color() {
        double r = f64(), g = f64(), b = f64(), a = f64();
        return Color::from_srgb(r, g, b, a);
    }
    PaintServer paint() {
        PaintServer ps;
        ps.kind = static_cast<PaintKind>(u8());
        ps.id = str();
        ps.solid_color = color();
        ps.start = vec2();
        ps.end = vec2();
        ps.center = vec2();
        ps.radius = f64();
        ps.angle_deg = f64();
        ps.user_space = (u8() != 0);
        uint32_t n_stops = u32();
        for (uint32_t i = 0; i < n_stops; ++i) {
            double off = f64();
            Color col = color();
            ps.stops.push_back({off, col});
        }
        ps.rows = static_cast<int>(u32());
        ps.cols = static_cast<int>(u32());
        uint32_t n_gp = u32();
        for (uint32_t i = 0; i < n_gp; ++i) {
            double u = f64(), v = f64();
            Color col = color();
            double w = f64();
            ps.grid_points.push_back({u, v, col, w});
        }
        ps.falloff_exponent = f64();
        uint32_t n_dp = u32();
        for (uint32_t i = 0; i < n_dp; ++i) {
            double u = f64(), v = f64();
            Color col = color();
            double w = f64();
            ps.diffusion_points.push_back({u, v, col, w});
        }
        ps.noise_scale = f64();
        ps.noise_octaves = static_cast<int>(u32());
        ps.noise_persistence = f64();
        ps.noise_lacunarity = f64();
        ps.noise_seed = f64();
        ps.grain = f64();
        return ps;
    }
    FontSpec font_spec() {
        FontSpec f;
        f.id = str();
        f.family = str();
        f.src = str();
        f.fallback = str();
        f.weight = f64();
        f.style = static_cast<FontStyle>(u8());
        f.stretch = f64();
        f.face_index = static_cast<int>(u32());
        return f;
    }
    FlattenedSubpath subpath() {
        FlattenedSubpath sp;
        sp.closed = (u8() != 0);
        uint32_t n_pts = u32();
        check_count(n_pts, 16, "subpath point");
        sp.points.reserve(n_pts);
        for (uint32_t i = 0; i < n_pts; ++i) sp.points.push_back(vec2());
        uint32_t n_len = u32();
        check_count(n_len, 8, "subpath length");
        sp.cumulative_lengths.reserve(n_len);
        for (uint32_t i = 0; i < n_len; ++i) sp.cumulative_lengths.push_back(f64());
        if (sp.cumulative_lengths.size() != sp.points.size()) {
            // Older/hand-edited payloads may omit or truncate the table.
            sp.cumulative_lengths.assign(sp.points.size(), 0.0);
            double acc = 0.0;
            for (size_t i = 0; i < sp.points.size(); ++i) {
                if (i > 0) acc += (sp.points[i] - sp.points[i - 1]).length();
                sp.cumulative_lengths[i] = acc;
            }
        }
        return sp;
    }
    ShapeNode node() {
        ShapeNode n;
        n.kind = static_cast<ShapeKind>(u8());
        n.id = str();
        n.style.fill = paint();
        n.style.stroke = paint();
        n.style.stroke_width = f64();
        n.style.stroke_align = static_cast<StrokeAlign>(u8());
        n.style.line_cap = static_cast<LineCap>(u8());
        n.style.line_join = static_cast<LineJoin>(u8());
        uint32_t n_dash = u32();
        for (uint32_t i = 0; i < n_dash; ++i) n.style.dash_array.push_back(f64());
        n.style.dash_offset = f64();
        n.style.opacity = f64();
        n.style.grain = f64();
        n.style.arrow_mode = static_cast<ArrowMode>(u8());
        n.style.arrow_size = f64();
        n.style.blend_mode = static_cast<BlendMode>(u8());
        n.style.clip_id = str();
        n.style.shadow.enabled = (u8() != 0);
        n.style.shadow.dx = f64(); n.style.shadow.dy = f64(); n.style.shadow.blur = f64();
        n.style.shadow.color = color();
        n.style.glow.enabled = (u8() != 0);
        n.style.glow.radius = f64();
        n.style.glow.color = color();

        // Node styles embed a copy of their paint server, so their interpolated
        // LUTs must be rebuilt after decoding; sampling an empty LUT yields black.
        n.style.fill.build_luts();
        n.style.stroke.build_luts();

        n.transform.a = f64(); n.transform.b = f64(); n.transform.c = f64();
        n.transform.d = f64(); n.transform.tx = f64(); n.transform.ty = f64();

        n.x = f64(); n.y = f64(); n.width = f64(); n.height = f64(); n.corner_radius = f64();
        for (int i = 0; i < 4; ++i) n.corner_radii[i] = f64();
        n.center = vec2(); n.radius = f64(); n.rx = f64(); n.ry = f64();
        n.from = vec2(); n.to = vec2();
        uint32_t n_pts = u32();
        for (uint32_t i = 0; i < n_pts; ++i) n.points.push_back(vec2());
        n.tension = f64();
        n.closed = (u8() != 0);

        uint32_t n_segs = u32();
        for (uint32_t i = 0; i < n_segs; ++i) {
            PathSegment seg;
            seg.verb = static_cast<PathVerb>(u8());
            seg.p1 = vec2(); seg.p2 = vec2(); seg.p3 = vec2();
            seg.radius = f64(); seg.bulge = f64();
            seg.clockwise = (u8() != 0);
            seg.large_arc = (u8() != 0);
            n.path_segments.push_back(seg);
        }

        n.bool_op = static_cast<BooleanOp>(u8());
        n.smoothness = f64();
        uint32_t n_ch = u32();
        for (uint32_t i = 0; i < n_ch; ++i) n.children.push_back(node());

        n.text_pos = vec2();
        n.text_content = str();
        n.font_size = f64();
        n.font_weight = f64();
        n.text_align = static_cast<TextAlign>(u8());
        n.letter_spacing = f64();

        if (version >= 5) {
            n.word_spacing = f64();
            n.font_id = str();
            n.font_family = str();
            n.font_src = str();
            n.font_fallback = str();
            n.font_features = str();
            n.font_language = str();
            n.font_style = static_cast<FontStyle>(u8());
            n.font_stretch = f64();
            n.font_weight_explicit = (u8() != 0);
            n.font_style_explicit = (u8() != 0);
            n.font_stretch_explicit = (u8() != 0);
            n.line_height = f64();
            n.max_width = f64();
            n.wrap = (u8() != 0);
            n.baseline_anchor = (u8() != 0);
            n.resolved_font_family = str();
            n.resolved_font_path = str();
            n.text_width = f64();
            n.text_ascent = f64();
            n.text_descent = f64();
            n.text_line_height = f64();
            n.text_line_count = static_cast<int>(u32());
            uint32_t n_sp = u32();
            check_count(n_sp, 9, "text subpath");
            n.text_subpaths.reserve(n_sp);
            for (uint32_t i = 0; i < n_sp; ++i) n.text_subpaths.push_back(subpath());
        }
        return n;
    }
};

Result encode_meshb(const Scene& scene, std::vector<uint8_t>& out_bytes) {
    std::vector<uint8_t> payload;
    BinWriter pw(payload);
    pw.str(scene.name);
    pw.u32(scene.width);
    pw.u32(scene.height);
    pw.u8(scene.has_background ? 1 : 0);
    pw.color(scene.background);
    pw.f64(scene.eval_time);

    pw.u32(static_cast<uint32_t>(scene.paint_servers.size()));
    for (const auto& [id, ps] : scene.paint_servers) {
        pw.paint(ps);
    }

    pw.u32(static_cast<uint32_t>(scene.nodes.size()));
    for (const auto& n : scene.nodes) {
        pw.node(n);
    }

    // --- v5 tail: scene font table + default typography + asset base dir. ---
    pw.u32(static_cast<uint32_t>(scene.fonts.size()));
    for (const auto& [id, spec] : scene.fonts) {
        (void)id;
        pw.font_spec(spec);
    }
    pw.str(scene.default_font_family);
    pw.str(scene.default_font_fallback);
    std::string stored_base_dir = scene.base_dir;
    if (!stored_base_dir.empty()) {
        std::error_code ec;
        std::filesystem::path base_path(stored_base_dir);
        if (!base_path.is_absolute()) base_path = std::filesystem::absolute(base_path, ec);
        if (!ec) stored_base_dir = base_path.lexically_normal().string();
    }
    pw.str(stored_base_dir);

    uint32_t crc = crc32_buf(payload.data(), payload.size());

    out_bytes.clear();
    out_bytes.reserve(16 + payload.size());
    BinWriter hw(out_bytes);
    hw.u8('M'); hw.u8('S'); hw.u8('H'); hw.u8('B');
    hw.u16(5); // Version 5: lossless real-font text + font table
    hw.u16(scene.has_background ? 1 : 0); // flags: bit0 = scene has background
    hw.u32(static_cast<uint32_t>(payload.size()));
    hw.u32(crc);
    out_bytes.insert(out_bytes.end(), payload.begin(), payload.end());
    return {true, "", 0, 0};
}

Result decode_meshb(const std::vector<uint8_t>& bytes, Scene& out_scene) {
    if (bytes.size() < 16) {
        return {false, "Invalid .meshb file: shorter than 16-byte header", 0, 0};
    }
    if (bytes[0] != 'M' || bytes[1] != 'S' || bytes[2] != 'H' || bytes[3] != 'B') {
        return {false, "Invalid .meshb magic header (expected 'MSHB')", 0, 0};
    }
    try {
        BinReader hr(bytes.data() + 4, bytes.size() - 4);
        uint16_t ver = hr.u16();
        if (ver != 5 && ver != 4 && ver != 1) {
            return {false, "Unsupported .meshb version: " + std::to_string(ver), 0, 0};
        }
        (void)hr.u16();
        uint32_t payload_len = hr.u32();
        uint32_t expected_crc = hr.u32();
        if (bytes.size() < 16 + payload_len) {
            return {false, "Corrupt .meshb payload length", 0, 0};
        }
        uint32_t actual_crc = crc32_buf(bytes.data() + 16, payload_len);
        if (actual_crc != expected_crc) {
            return {false, "CRC32 checksum mismatch in .meshb file", 0, 0};
        }

        BinReader pr(bytes.data() + 16, payload_len);
        pr.version = ver;
        out_scene = Scene{};
        out_scene.name = pr.str();
        out_scene.width = pr.u32();
        out_scene.height = pr.u32();
        out_scene.has_background = (pr.u8() != 0);
        out_scene.background = pr.color();
        out_scene.eval_time = pr.f64();

        uint32_t n_ps = pr.u32();
        for (uint32_t i = 0; i < n_ps; ++i) {
            PaintServer ps = pr.paint();
            ps.build_luts();
            out_scene.paint_servers[ps.id] = ps;
        }

        uint32_t n_nodes = pr.u32();
        if (n_nodes != 0 && static_cast<size_t>(n_nodes) > (payload_len - pr.pos) / 9) {
            return {false, "Corrupt .meshb: node count exceeds payload size", 0, 0};
        }
        for (uint32_t i = 0; i < n_nodes; ++i) {
            out_scene.nodes.push_back(pr.node());
        }

        if (ver >= 5) {
            uint32_t n_fonts = pr.u32();
            pr.check_count(n_fonts, 4 * sizeof(uint32_t) + sizeof(double) + 1 + sizeof(double) + sizeof(uint32_t),
                           "font table");
            for (uint32_t i = 0; i < n_fonts; ++i) {
                FontSpec spec = pr.font_spec();
                out_scene.fonts[spec.id] = spec;
            }
            out_scene.default_font_family = pr.str();
            out_scene.default_font_fallback = pr.str();
            out_scene.base_dir = pr.str();
        }

        // Packed v5 scenes normally carry shaped outlines, so decoding does not
        // need the machine's font database. If a v5 payload has no outlines at
        // all (e.g. it was written by a build without a font backend), lay the
        // text out now that the font table and base_dir are restored.
        bool has_text = false;
        bool has_outlines = false;
        std::function<void(const ShapeNode&)> scan_text = [&](const ShapeNode& node) {
            if (node.kind == ShapeKind::Text && !node.text_content.empty()) {
                has_text = true;
                if (!node.text_subpaths.empty()) has_outlines = true;
            }
            for (const auto& child : node.children) scan_text(child);
        };
        for (const auto& node : out_scene.nodes) scan_text(node);
        if (has_text && !has_outlines) {
            Result lr = layout_scene_text(out_scene);
            if (!lr.ok) {
                return {false, "Failed to lay out text while decoding .meshb: " + lr.error, 0, 0};
            }
        }

        recompute_scene_Caches(out_scene);
        return {true, "", 0, 0};
    } catch (const std::exception& e) {
        return {false, e.what(), 0, 0};
    }
}

Result write_meshb_file(const Scene& scene, const std::string& path) {
    std::vector<uint8_t> bin;
    Result r = encode_meshb(scene, bin);
    if (!r.ok) return r;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return {false, "Cannot open .meshb output file: " + path, 0, 0};
    ofs.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    return {true, "", 0, 0};
}

Result read_meshb_file(const std::string& path, Scene& out_scene) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {false, "Cannot open .meshb input file: " + path, 0, 0};
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return decode_meshb(buf, out_scene);
}

static std::string fmt_num(double v, int precision);
static std::string mesh_quote(const std::string& s);

static std::string decompile_color_text(const Color& c) {
    if (c.a < 0.999) return "fade(" + c.to_hex() + ", " + fmt_num(c.a, 6) + ")";
    return c.to_hex();
}

static void decompile_paint_server(const PaintServer& ps, std::ostringstream& out) {
    const char* kw = "linear_gradient";
    if (ps.kind == PaintKind::RadialGradient) kw = "radial_gradient";
    else if (ps.kind == PaintKind::ConicGradient) kw = "conic_gradient";
    else if (ps.kind == PaintKind::NoiseGradient) kw = "noise_gradient";
    else if (ps.kind == PaintKind::DiffusionGradient) kw = "diffusion_gradient";
    else if (ps.kind == PaintKind::MeshGradient) kw = "mesh_gradient";

    out << "  " << kw << " \"" << mesh_quote(ps.id) << "\" {\n";
    if (ps.user_space) out << "    space: world\n";
    if (ps.kind == PaintKind::LinearGradient) {
        out << "    from: (" << fmt_num(ps.start.x, 6) << ", " << fmt_num(ps.start.y, 6) << ")\n"
            << "    to: (" << fmt_num(ps.end.x, 6) << ", " << fmt_num(ps.end.y, 6) << ")\n";
    } else if (ps.kind == PaintKind::RadialGradient) {
        out << "    center: (" << fmt_num(ps.center.x, 6) << ", " << fmt_num(ps.center.y, 6) << ")\n"
            << "    radius: " << fmt_num(ps.radius, 6) << "\n";
    } else if (ps.kind == PaintKind::ConicGradient) {
        out << "    center: (" << fmt_num(ps.center.x, 6) << ", " << fmt_num(ps.center.y, 6) << ")\n"
            << "    angle: " << fmt_num(ps.angle_deg, 6) << "\n";
    } else if (ps.kind == PaintKind::NoiseGradient) {
        out << "    scale: " << fmt_num(ps.noise_scale, 6) << "\n"
            << "    octaves: " << ps.noise_octaves << "\n"
            << "    persistence: " << fmt_num(ps.noise_persistence, 6) << "\n"
            << "    lacunarity: " << fmt_num(ps.noise_lacunarity, 6) << "\n"
            << "    seed: " << fmt_num(ps.noise_seed, 6) << "\n";
    } else if (ps.kind == PaintKind::MeshGradient) {
        out << "    rows: " << ps.rows << "\n"
            << "    cols: " << ps.cols << "\n";
    } else if (ps.kind == PaintKind::DiffusionGradient) {
        out << "    falloff: " << fmt_num(ps.falloff_exponent, 6) << "\n";
    }
    if (ps.grain > 0.0) out << "    grain: " << fmt_num(ps.grain, 6) << "\n";
    for (const auto& st : ps.stops) {
        out << "    stop(" << fmt_num(st.offset, 6) << ", " << decompile_color_text(st.color) << ")\n";
    }
    for (const auto& dp : ps.diffusion_points) {
        out << "    point(" << fmt_num(dp.u, 6) << ", " << fmt_num(dp.v, 6) << ", "
            << decompile_color_text(dp.color) << ", " << fmt_num(dp.weight, 6) << ")\n";
    }
    for (const auto& gp : ps.grid_points) {
        out << "    patch(" << fmt_num(gp.u, 6) << ", " << fmt_num(gp.v, 6) << ", "
            << decompile_color_text(gp.color) << ")\n";
    }
    out << "  }\n\n";
}

static std::string mesh_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

static std::string fmt_num(double v, int precision) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << v;
    return ss.str();
}

static const char* font_style_name(FontStyle style) {
    if (style == FontStyle::Italic) return "italic";
    if (style == FontStyle::Oblique) return "oblique";
    return "normal";
}

static std::string decompile_asset_path(const std::string& src, const std::string& base_dir) {
    if (src.empty() || base_dir.empty()) return src;
    std::error_code ec;
    std::filesystem::path path(src);
    if (path.is_absolute()) return path.lexically_normal().string();
    std::filesystem::path resolved = std::filesystem::path(base_dir) / path;
    if (!resolved.is_absolute()) resolved = std::filesystem::absolute(resolved, ec);
    if (ec) return src;
    return resolved.lexically_normal().string();
}

static void decompile_font_spec(const FontSpec& f, std::ostringstream& out,
                                const std::string& base_dir) {
    out << "  font \"" << mesh_quote(f.id) << "\" {\n";
    if (!f.family.empty()) out << "    family: \"" << mesh_quote(f.family) << "\"\n";
    if (!f.src.empty()) out << "    src: \"" << mesh_quote(decompile_asset_path(f.src, base_dir)) << "\"\n";
    if (!f.fallback.empty()) out << "    fallback: \"" << mesh_quote(f.fallback) << "\"\n";
    if (std::abs(f.weight - 400.0) > 1e-9) out << "    weight: " << fmt_num(f.weight, 3) << "\n";
    if (f.style != FontStyle::Normal) out << "    style: " << font_style_name(f.style) << "\n";
    if (std::abs(f.stretch - 100.0) > 1e-9) out << "    stretch: " << fmt_num(f.stretch, 3) << "\n";
    if (f.face_index != 0) out << "    index: " << f.face_index << "\n";
    out << "  }\n\n";
}

static void decompile_node(const ShapeNode& n, std::ostringstream& out, int indent,
                           const std::string& base_dir) {
    std::string pad(indent * 2, ' ');
    const char* kw = "rect";
    switch (n.kind) {
        case ShapeKind::Rect: kw = "rect"; break;
        case ShapeKind::Circle: kw = "circle"; break;
        case ShapeKind::Ellipse: kw = "ellipse"; break;
        case ShapeKind::Line: kw = "line"; break;
        case ShapeKind::Polygon: kw = "polygon"; break;
        case ShapeKind::Polyline: kw = "polyline"; break;
        case ShapeKind::Spline: kw = "spline"; break;
        case ShapeKind::Path: kw = "path"; break;
        case ShapeKind::Boolean: kw = "boolean"; break;
        case ShapeKind::Text: kw = "text"; break;
        case ShapeKind::Group: kw = "group"; break;
    }
    out << pad << kw;
    if (!n.id.empty()) out << " \"" << mesh_quote(n.id) << "\"";
    out << " {\n";
    std::string ip(indent * 2 + 2, ' ');

    if (n.kind == ShapeKind::Rect) {
        out << ip << "x: " << n.x << "\n" << ip << "y: " << n.y << "\n"
            << ip << "width: " << n.width << "\n" << ip << "height: " << n.height << "\n";
        if (n.corner_radius > 0) out << ip << "radius: " << n.corner_radius << "\n";
    } else if (n.kind == ShapeKind::Circle) {
        out << ip << "center: (" << n.center.x << ", " << n.center.y << ")\n"
            << ip << "radius: " << n.radius << "\n";
    } else if (n.kind == ShapeKind::Ellipse) {
        out << ip << "center: (" << n.center.x << ", " << n.center.y << ")\n"
            << ip << "rx: " << n.rx << "\n" << ip << "ry: " << n.ry << "\n";
    } else if (n.kind == ShapeKind::Line) {
        out << ip << "from: (" << n.from.x << ", " << n.from.y << ")\n"
            << ip << "to: (" << n.to.x << ", " << n.to.y << ")\n";
    } else if (n.kind == ShapeKind::Polygon || n.kind == ShapeKind::Polyline || n.kind == ShapeKind::Spline) {
        out << ip << "points: [";
        for (size_t i = 0; i < n.points.size(); ++i) {
            if (i > 0) out << ", ";
            out << "(" << n.points[i].x << ", " << n.points[i].y << ")";
        }
        out << "]\n";
        if (n.kind == ShapeKind::Spline) out << ip << "tension: " << n.tension << "\n";
    } else if (n.kind == ShapeKind::Path) {
        for (const auto& s : n.path_segments) {
            if (s.verb == PathVerb::MoveTo) out << ip << "move(" << s.p1.x << ", " << s.p1.y << ")\n";
            else if (s.verb == PathVerb::LineTo) out << ip << "line(" << s.p1.x << ", " << s.p1.y << ")\n";
            else if (s.verb == PathVerb::QuadTo) out << ip << "quad((" << s.p1.x << ", " << s.p1.y << "), (" << s.p2.x << ", " << s.p2.y << "))\n";
            else if (s.verb == PathVerb::CubicTo) out << ip << "cubic((" << s.p1.x << ", " << s.p1.y << "), (" << s.p2.x << ", " << s.p2.y << "), (" << s.p3.x << ", " << s.p3.y << "))\n";
            else if (s.verb == PathVerb::Close) out << ip << "close\n";
        }
    } else if (n.kind == ShapeKind::Boolean) {
        if (n.bool_op == BooleanOp::Subtract) out << ip << "op: subtract\n";
        else if (n.bool_op == BooleanOp::Intersect) out << ip << "op: intersect\n";
        else if (n.bool_op == BooleanOp::SmoothUnion) out << ip << "op: smooth_union(" << n.smoothness << ")\n";
        else out << ip << "op: union\n";
    } else if (n.kind == ShapeKind::Text) {
        out << ip << "at: (" << fmt_num(n.text_pos.x, 6) << ", " << fmt_num(n.text_pos.y, 6) << ")\n";
        if (n.baseline_anchor) out << ip << "baseline: true\n";
        out << ip << "content: \"" << mesh_quote(n.text_content) << "\"\n"
            << ip << "size: " << fmt_num(n.font_size, 6) << "\n";
        if (n.text_align == TextAlign::Center) out << ip << "align: center\n";
        else if (n.text_align == TextAlign::Right) out << ip << "align: right\n";
        if (n.font_weight_explicit || std::abs(n.font_weight - 400.0) > 1e-9) {
            out << ip << "weight: " << fmt_num(n.font_weight, 3) << "\n";
        }
        if (std::abs(n.letter_spacing) > 1e-9) out << ip << "letter_spacing: " << fmt_num(n.letter_spacing, 6) << "\n";
        if (std::abs(n.word_spacing) > 1e-9) out << ip << "word_spacing: " << fmt_num(n.word_spacing, 6) << "\n";
        if (!n.font_id.empty()) out << ip << "font: \"" << mesh_quote(n.font_id) << "\"\n";
        if (!n.font_family.empty()) out << ip << "font_family: \"" << mesh_quote(n.font_family) << "\"\n";
        if (!n.font_src.empty()) out << ip << "font_src: \"" << mesh_quote(decompile_asset_path(n.font_src, base_dir)) << "\"\n";
        if (!n.font_fallback.empty()) out << ip << "font_fallback: \"" << mesh_quote(n.font_fallback) << "\"\n";
        if (n.font_style_explicit) out << ip << "font_style: " << font_style_name(n.font_style) << "\n";
        if (n.font_stretch_explicit) out << ip << "font_stretch: " << fmt_num(n.font_stretch, 3) << "\n";
        if (!n.font_features.empty()) out << ip << "font_features: \"" << mesh_quote(n.font_features) << "\"\n";
        if (!n.font_language.empty()) out << ip << "font_language: \"" << mesh_quote(n.font_language) << "\"\n";
        if (std::abs(n.line_height - 1.25) > 1e-9) out << ip << "line_height: " << fmt_num(n.line_height, 6) << "\n";
        if (n.max_width > 0.0) out << ip << "max_width: " << fmt_num(n.max_width, 6) << "\n";
        if (n.wrap) out << ip << "wrap: true\n";
    }

    if (!n.transform.is_identity()) {
        out << ip << "matrix: (" << fmt_num(n.transform.a, 6) << ", " << fmt_num(n.transform.b, 6) << ", "
            << fmt_num(n.transform.c, 6) << ", " << fmt_num(n.transform.d, 6) << ", "
            << fmt_num(n.transform.tx, 6) << ", " << fmt_num(n.transform.ty, 6) << ")\n";
    }

    if (n.style.fill.kind == PaintKind::Solid) {
        out << ip << "fill: " << decompile_color_text(n.style.fill.solid_color) << "\n";
    } else if (n.style.fill.kind != PaintKind::None && !n.style.fill.id.empty()) {
        out << ip << "fill: @" << n.style.fill.id << "\n";
    }
    if (n.style.stroke.kind == PaintKind::Solid && n.style.stroke_width > 0) {
        out << ip << "stroke: " << decompile_color_text(n.style.stroke.solid_color) << "\n"
            << ip << "stroke_width: " << n.style.stroke_width << "\n";
    } else if (n.style.stroke.kind != PaintKind::None && !n.style.stroke.id.empty() && n.style.stroke_width > 0) {
        out << ip << "stroke: @" << n.style.stroke.id << "\n"
            << ip << "stroke_width: " << n.style.stroke_width << "\n";
    }
    if (!n.style.dash_array.empty()) {
        out << ip << "dash: [";
        for (size_t i = 0; i < n.style.dash_array.size(); ++i) {
            if (i > 0) out << ", ";
            out << n.style.dash_array[i];
        }
        out << "]\n";
        if (n.style.dash_offset != 0.0) out << ip << "dash_offset: " << n.style.dash_offset << "\n";
    }
    if (n.style.arrow_mode == ArrowMode::End) out << ip << "arrow: end\n" << ip << "arrow_size: " << n.style.arrow_size << "\n";
    else if (n.style.arrow_mode == ArrowMode::Start) out << ip << "arrow: start\n" << ip << "arrow_size: " << n.style.arrow_size << "\n";
    else if (n.style.arrow_mode == ArrowMode::Both) out << ip << "arrow: both\n" << ip << "arrow_size: " << n.style.arrow_size << "\n";
    if (n.style.opacity < 0.999) out << ip << "opacity: " << n.style.opacity << "\n";
    if (n.style.stroke_align == StrokeAlign::Inner) out << ip << "stroke_align: inner\n";
    else if (n.style.stroke_align == StrokeAlign::Outer) out << ip << "stroke_align: outer\n";
    if (n.style.blend_mode == BlendMode::Add) out << ip << "blend: add\n";
    else if (n.style.blend_mode == BlendMode::Screen) out << ip << "blend: screen\n";
    else if (n.style.blend_mode == BlendMode::Multiply) out << ip << "blend: multiply\n";
    else if (n.style.blend_mode == BlendMode::Overlay) out << ip << "blend: overlay\n";
    if (n.style.shadow.enabled) {
        out << ip << "shadow(" << n.style.shadow.dx << ", " << n.style.shadow.dy << ", "
            << n.style.shadow.blur << ", ";
        if (n.style.shadow.color.a < 0.999) {
            out << "fade(" << n.style.shadow.color.to_hex() << ", " << fmt_num(n.style.shadow.color.a, 6) << ")";
        } else {
            out << n.style.shadow.color.to_hex();
        }
        out << ")\n";
    }
    if (n.style.glow.enabled) {
        out << ip << "glow(" << n.style.glow.radius << ", ";
        if (n.style.glow.color.a < 0.999) {
            out << "fade(" << n.style.glow.color.to_hex() << ", " << fmt_num(n.style.glow.color.a, 6) << ")";
        } else {
            out << n.style.glow.color.to_hex();
        }
        out << ")\n";
    }
    if (n.style.grain > 0.0) out << ip << "grain: " << fmt_num(n.style.grain, 6) << "\n";
    if (!n.style.clip_id.empty()) out << ip << "clip: @" << n.style.clip_id << "\n";

    for (const auto& ch : n.children) {
        decompile_node(ch, out, indent + 1, base_dir);
    }
    out << pad << "}\n";
}

Result decompile_scene_to_mesh(const Scene& scene, std::string& out_mesh_source) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "mesh \"" << mesh_quote(scene.name) << "\" {\n";
    oss << "  canvas {\n    width: " << scene.width << "\n    height: " << scene.height << "\n";
    if (scene.has_background) {
        oss << "    background: " << decompile_color_text(scene.background) << "\n";
    }
    if (!scene.default_font_family.empty() && scene.default_font_family != "Noto Sans") {
        oss << "    font_family: \"" << mesh_quote(scene.default_font_family) << "\"\n";
    }
    if (!scene.default_font_fallback.empty() && scene.default_font_fallback != "DejaVu Sans") {
        oss << "    font_fallback: \"" << mesh_quote(scene.default_font_fallback) << "\"\n";
    }
    oss << "  }\n\n";
    for (const auto& [id, spec] : scene.fonts) {
        (void)id;
        decompile_font_spec(spec, oss, scene.base_dir);
    }
    for (const auto& [id, ps] : scene.paint_servers) {
        decompile_paint_server(ps, oss);
    }
    for (const auto& n : scene.nodes) {
        decompile_node(n, oss, 1, scene.base_dir);
    }
    oss << "}\n";
    out_mesh_source = oss.str();
    return {true, "", 0, 0};
}

// ============================================================================
// 3. Final Beta Exact Analytical WebGPU WGSL & Vulkan/GLSL 450 Shader Compilers
// ============================================================================

static void collect_shader_leaves(const ShapeNode& n, std::vector<const ShapeNode*>& leaves) {
    if (n.kind == ShapeKind::Group) {
        for (const auto& ch : n.children) collect_shader_leaves(ch, leaves);
    } else if (n.kind != ShapeKind::Text) {
        leaves.push_back(&n);
    }
}

static void emit_wgsl_shape_sdf(const ShapeNode& node, const std::string& var_name, int& sub_id, std::ostringstream& w) {
    if (node.kind == ShapeKind::Boolean && !node.children.empty()) {
        std::string v0 = var_name + "_c0_" + std::to_string(++sub_id);
        emit_wgsl_shape_sdf(node.children[0], v0, sub_id, w);
        std::string cur = v0;
        for (size_t i = 1; i < node.children.size(); ++i) {
            std::string vi = var_name + "_c" + std::to_string(i) + "_" + std::to_string(++sub_id);
            emit_wgsl_shape_sdf(node.children[i], vi, sub_id, w);
            std::string next_v = var_name + "_m" + std::to_string(i) + "_" + std::to_string(++sub_id);
            if (node.bool_op == BooleanOp::Union) {
                w << "  let " << next_v << " = min(" << cur << ", " << vi << ");\n";
            } else if (node.bool_op == BooleanOp::Subtract) {
                w << "  let " << next_v << " = max(" << cur << ", -" << vi << ");\n";
            } else if (node.bool_op == BooleanOp::Intersect) {
                w << "  let " << next_v << " = max(" << cur << ", " << vi << ");\n";
            } else {
                w << "  let " << next_v << " = smin_sdf(" << cur << ", " << vi << ", " << std::max(0.001, node.smoothness) << ");\n";
            }
            cur = next_v;
        }
        w << "  let " << var_name << " = " << cur << ";\n";
        return;
    }

    int loc_id = ++sub_id;
    std::string p_loc = "pl_" + std::to_string(loc_id);
    w << "  let " << p_loc << " = vec2<f32>("
      << node.inv_transform.a << " * p.x + " << node.inv_transform.c << " * p.y + " << node.inv_transform.tx << ", "
      << node.inv_transform.b << " * p.x + " << node.inv_transform.d << " * p.y + " << node.inv_transform.ty << ");\n";

    if (node.kind == ShapeKind::Rect) {
        double cx = node.x + node.width * 0.5;
        double cy = node.y + node.height * 0.5;
        w << "  let " << var_name << " = sd_rounded_box4(" << p_loc << " - vec2<f32>(" << cx << ", " << cy
          << "), vec2<f32>(" << (node.width * 0.5) << ", " << (node.height * 0.5) << "), vec4<f32>("
          << node.corner_radii[0] << ", " << node.corner_radii[1] << ", " << node.corner_radii[2] << ", " << node.corner_radii[3]
          << ")) * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Circle) {
        w << "  let " << var_name << " = sd_circle(" << p_loc << ", vec2<f32>(" << node.center.x << ", " << node.center.y
          << "), " << node.radius << ") * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Ellipse) {
        w << "  let " << var_name << " = sd_ellipse(" << p_loc << " - vec2<f32>(" << node.center.x << ", " << node.center.y
          << "), vec2<f32>(" << std::max(1e-3, node.rx) << ", " << std::max(1e-3, node.ry) << ")) * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Line) {
        w << "  let " << var_name << " = sd_segment(" << p_loc << ", vec2<f32>(" << node.from.x << ", " << node.from.y
          << "), vec2<f32>(" << node.to.x << ", " << node.to.y << ")) * " << node.scale_factor << ";\n";
    } else {
        // Exact segment/polygon SDF for Polygon, Polyline, Spline, Path
        std::vector<Vec2> pts = node.points;
        bool is_closed = node.closed || (node.kind == ShapeKind::Polygon);
        if ((node.kind == ShapeKind::Spline || node.kind == ShapeKind::Path) && !node.subpaths.empty()) {
            pts = node.subpaths.front().points;
            is_closed = node.subpaths.front().closed || (node.style.fill.kind != PaintKind::None);
        }
        if (pts.size() >= 2) {
            std::string d_acc = "d_acc_" + std::to_string(loc_id);
            w << "  var " << d_acc << " = 1e9;\n";
            if (is_closed) {
                std::string s_acc = "s_acc_" + std::to_string(loc_id);
                w << "  var " << s_acc << " = 1.0;\n";
                for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
                    w << "  " << d_acc << " = min(" << d_acc << ", sd_segment(" << p_loc << ", vec2<f32>("
                      << pts[j].x << ", " << pts[j].y << "), vec2<f32>(" << pts[i].x << ", " << pts[i].y << ")));\n";
                    w << "  " << s_acc << " *= poly_winding_step(" << p_loc << ", vec2<f32>("
                      << pts[j].x << ", " << pts[j].y << "), vec2<f32>(" << pts[i].x << ", " << pts[i].y << "));\n";
                }
                w << "  let " << var_name << " = " << s_acc << " * " << d_acc << " * " << node.scale_factor << ";\n";
            } else {
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    w << "  " << d_acc << " = min(" << d_acc << ", sd_segment(" << p_loc << ", vec2<f32>("
                      << pts[i].x << ", " << pts[i].y << "), vec2<f32>(" << pts[i + 1].x << ", " << pts[i + 1].y << ")));\n";
                }
                w << "  let " << var_name << " = " << d_acc << " * " << node.scale_factor << ";\n";
            }
        } else {
            w << "  let " << var_name << " = 1e9;\n";
        }
    }
}

Result compile_to_wgsl(const Scene& scene, std::string& out_wgsl) {
    std::ostringstream w;
    w << std::fixed << std::setprecision(4);
    w << "// ============================================================================\n";
    w << "// Generated by Mesh GPU Shader Compiler (meshc v" << VERSION << " --target wgsl)\n";
    w << "// Scene: \"" << scene.name << "\" (" << scene.width << "x" << scene.height << ")\n";
    w << "// Pipeline: Exact Analytical SDF + Boolean CSG + OKLab FBM Noise Fragment Shader\n";
    w << "// ============================================================================\n\n";

    w << R"WGSL(struct MeshUniforms {
  resolution: vec2<f32>,
  time: f32,
  zoom: f32,
  pan: vec2<f32>,
  pad: vec2<f32>,
};

@group(0) @binding(0) var<uniform> u: MeshUniforms;

struct VertexOut {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) v_idx: u32) -> VertexOut {
  var quad = array<vec2<f32>, 6>(
    vec2<f32>(-1.0, -1.0), vec2<f32>( 1.0, -1.0), vec2<f32>(-1.0,  1.0),
    vec2<f32>(-1.0,  1.0), vec2<f32>( 1.0, -1.0), vec2<f32>( 1.0,  1.0)
  );
  var out: VertexOut;
  out.position = vec4<f32>(quad[v_idx], 0.0, 1.0);
  out.uv = vec2<f32>(quad[v_idx].x * 0.5 + 0.5, 0.5 - quad[v_idx].y * 0.5);
  return out;
}

fn sd_rounded_box4(p: vec2<f32>, b: vec2<f32>, r4: vec4<f32>) -> f32 {
  var r = select(select(r4.x, r4.w, p.y > 0.0), select(r4.y, r4.z, p.y > 0.0), p.x > 0.0);
  r = min(r, min(b.x, b.y));
  let q = abs(p) - b + vec2<f32>(r, r);
  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;
}

fn sd_circle(p: vec2<f32>, c: vec2<f32>, r: f32) -> f32 {
  return length(p - c) - r;
}

fn sd_ellipse(p: vec2<f32>, ab: vec2<f32>) -> f32 {
  let q = p / ab;
  let r = length(q);
  let grad = length(p / (ab * ab));
  return (r - 1.0) * (r / max(grad, 1e-6));
}

fn sd_segment(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let pa = p - a;
  let ba = b - a;
  let h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
  return length(pa - ba * h);
}

fn poly_winding_step(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let e = b - a;
  let w = p - a;
  let c1 = p.y >= a.y;
  let c2 = p.y < b.y;
  let c3 = (e.x * w.y > e.y * w.x);
  if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) {
    return -1.0;
  }
  return 1.0;
}

fn smin_sdf(a: f32, b: f32, k: f32) -> f32 {
  let h = clamp(0.5 + 0.5 * (b - a) / max(k, 1e-4), 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}

fn hash21(p: vec2<f32>) -> f32 {
  var p3 = fract(vec3<f32>(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + vec3<f32>(33.33));
  return fract((p3.x + p3.y) * p3.z);
}

fn fbm_noise(p: vec2<f32>) -> f32 {
  var v = 0.0;
  var a = 0.5;
  var pos = p;
  for (var i = 0; i < 4; i = i + 1) {
    let i_pos = floor(pos);
    let f_pos = fract(pos);
    let u_interp = f_pos * f_pos * (3.0 - 2.0 * f_pos);
    let n = mix(
      mix(hash21(i_pos + vec2<f32>(0.0, 0.0)), hash21(i_pos + vec2<f32>(1.0, 0.0)), u_interp.x),
      mix(hash21(i_pos + vec2<f32>(0.0, 1.0)), hash21(i_pos + vec2<f32>(1.0, 1.0)), u_interp.x),
      u_interp.y
    );
    v += a * n;
    pos = pos * 2.02;
    a *= 0.5;
  }
  return v;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
  let p = (in.uv * vec2<f32>()WGSL" << scene.width << ".0, " << scene.height << R"WGSL(.0) - u.pan) / max(u.zoom, 0.05);
)WGSL";

    Color bg = scene.has_background ? scene.background : Color::from_srgb(0.05, 0.07, 0.10, 1.0);
    w << "  var col = vec4<f32>(" << bg.r << ", " << bg.g << ", " << bg.b << ", " << bg.a << ");\n";

    std::vector<const ShapeNode*> leaves;
    for (const auto& n : scene.nodes) collect_shader_leaves(n, leaves);

    int idx = 0;
    int sub_id = 0;
    for (const ShapeNode* leaf : leaves) {
        ++idx;
        std::string v_d = "d_" + std::to_string(idx);
        w << "\n  // Node #" << idx << " (" << (leaf->id.empty() ? "anon" : leaf->id) << ")\n";
        emit_wgsl_shape_sdf(*leaf, v_d, sub_id, w);

        bool open_curve = (leaf->kind == ShapeKind::Line || leaf->kind == ShapeKind::Polyline ||
                           ((leaf->kind == ShapeKind::Spline || leaf->kind == ShapeKind::Path) &&
                            !leaf->closed && leaf->style.fill.kind == PaintKind::None));

        if (!open_curve && leaf->style.fill.kind != PaintKind::None) {
            std::string fc_var = "fc_" + std::to_string(idx);
            if (leaf->style.fill.kind == PaintKind::NoiseGradient && leaf->style.fill.stops.size() >= 2) {
                Color c0 = leaf->style.fill.stops.front().color;
                Color c1 = leaf->style.fill.stops.back().color;
                double bw = std::max(1.0, leaf->bbox.width());
                double bh = std::max(1.0, leaf->bbox.height());
                w << "  let n_" << idx << " = fbm_noise(vec2<f32>((p.x - " << leaf->bbox.min_x << ") / " << bw
                  << ", (p.y - " << leaf->bbox.min_y << ") / " << bh << ") * " << leaf->style.fill.noise_scale << ");\n";
                w << "  let " << fc_var << " = mix(vec3<f32>(" << c0.r << ", " << c0.g << ", " << c0.b
                  << "), vec3<f32>(" << c1.r << ", " << c1.g << ", " << c1.b << "), n_" << idx << ");\n";
            } else if (leaf->style.fill.kind == PaintKind::LinearGradient && leaf->style.fill.stops.size() >= 2) {
                Color c0 = leaf->style.fill.stops.front().color;
                Color c1 = leaf->style.fill.stops.back().color;
                double bw = std::max(1.0, leaf->bbox.width());
                w << "  let lt_" << idx << " = clamp((p.x - " << leaf->bbox.min_x << ") / " << bw << ", 0.0, 1.0);\n";
                w << "  let " << fc_var << " = mix(vec3<f32>(" << c0.r << ", " << c0.g << ", " << c0.b
                  << "), vec3<f32>(" << c1.r << ", " << c1.g << ", " << c1.b << "), lt_" << idx << ");\n";
            } else {
                Color fc = leaf->style.fill.sample(leaf->bbox.center(), leaf->bbox);
                w << "  let " << fc_var << " = vec3<f32>(" << fc.r << ", " << fc.g << ", " << fc.b << ");\n";
            }
            Color fc_mid = leaf->style.fill.sample(leaf->bbox.center(), leaf->bbox);
            w << "  let cov_f_" << idx << " = clamp(0.5 - " << v_d << ", 0.0, 1.0) * " << (fc_mid.a * leaf->style.opacity) << ";\n";
            w << "  col = vec4<f32>(mix(col.rgb, " << fc_var << ", cov_f_" << idx << "), max(col.a, cov_f_" << idx << "));\n";
        }
        if (leaf->style.stroke.kind != PaintKind::None && leaf->style.stroke_width > 0.0) {
            Color sc = leaf->style.stroke.sample(leaf->bbox.center(), leaf->bbox);
            w << "  let cov_s_" << idx << " = clamp(0.5 - (abs(" << v_d << ") - " << (leaf->style.stroke_width * 0.5) << "), 0.0, 1.0) * " << (sc.a * leaf->style.opacity) << ";\n";
            w << "  col = vec4<f32>(mix(col.rgb, vec3<f32>(" << sc.r << ", " << sc.g << ", " << sc.b << "), cov_s_" << idx << "), max(col.a, cov_s_" << idx << "));\n";
        }
    }

    w << "\n  return col;\n}\n";
    out_wgsl = w.str();
    return {true, "", 0, 0};
}

static void emit_glsl_shape_sdf(const ShapeNode& node, const std::string& var_name, int& sub_id, std::ostringstream& g) {
    if (node.kind == ShapeKind::Boolean && !node.children.empty()) {
        std::string v0 = var_name + "_c0_" + std::to_string(++sub_id);
        emit_glsl_shape_sdf(node.children[0], v0, sub_id, g);
        std::string cur = v0;
        for (size_t i = 1; i < node.children.size(); ++i) {
            std::string vi = var_name + "_c" + std::to_string(i) + "_" + std::to_string(++sub_id);
            emit_glsl_shape_sdf(node.children[i], vi, sub_id, g);
            std::string next_v = var_name + "_m" + std::to_string(i) + "_" + std::to_string(++sub_id);
            if (node.bool_op == BooleanOp::Union) {
                g << "    float " << next_v << " = min(" << cur << ", " << vi << ");\n";
            } else if (node.bool_op == BooleanOp::Subtract) {
                g << "    float " << next_v << " = max(" << cur << ", -" << vi << ");\n";
            } else if (node.bool_op == BooleanOp::Intersect) {
                g << "    float " << next_v << " = max(" << cur << ", " << vi << ");\n";
            } else {
                g << "    float " << next_v << " = sminSdf(" << cur << ", " << vi << ", " << std::max(0.001, node.smoothness) << ");\n";
            }
            cur = next_v;
        }
        g << "    float " << var_name << " = " << cur << ";\n";
        return;
    }

    int loc_id = ++sub_id;
    std::string p_loc = "pl_" + std::to_string(loc_id);
    g << "    vec2 " << p_loc << " = vec2("
      << node.inv_transform.a << " * p.x + " << node.inv_transform.c << " * p.y + " << node.inv_transform.tx << ", "
      << node.inv_transform.b << " * p.x + " << node.inv_transform.d << " * p.y + " << node.inv_transform.ty << ");\n";

    if (node.kind == ShapeKind::Rect) {
        double cx = node.x + node.width * 0.5;
        double cy = node.y + node.height * 0.5;
        g << "    float " << var_name << " = sdRoundedBox(" << p_loc << " - vec2(" << cx << ", " << cy << "), vec2("
          << (node.width * 0.5) << ", " << (node.height * 0.5) << "), " << node.corner_radius << ") * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Circle) {
        g << "    float " << var_name << " = sdCircle(" << p_loc << ", vec2(" << node.center.x << ", " << node.center.y << "), " << node.radius << ") * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Ellipse) {
        g << "    float " << var_name << " = sdEllipse(" << p_loc << " - vec2(" << node.center.x << ", " << node.center.y
          << "), vec2(" << std::max(1e-3, node.rx) << ", " << std::max(1e-3, node.ry) << ")) * " << node.scale_factor << ";\n";
    } else if (node.kind == ShapeKind::Line) {
        g << "    float " << var_name << " = sdSegment(" << p_loc << ", vec2(" << node.from.x << ", " << node.from.y << "), vec2(" << node.to.x << ", " << node.to.y << ")) * " << node.scale_factor << ";\n";
    } else {
        std::vector<Vec2> pts = node.points;
        if ((node.kind == ShapeKind::Spline || node.kind == ShapeKind::Path) && !node.subpaths.empty()) {
            pts = node.subpaths.front().points;
        }
        if (pts.size() >= 2) {
            std::string d_acc = "d_acc_" + std::to_string(loc_id);
            g << "    float " << d_acc << " = 1e9;\n";
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                g << "    " << d_acc << " = min(" << d_acc << ", sdSegment(" << p_loc << ", vec2("
                  << pts[i].x << ", " << pts[i].y << "), vec2(" << pts[i + 1].x << ", " << pts[i + 1].y << ")));\n";
            }
            g << "    float " << var_name << " = " << d_acc << " * " << node.scale_factor << ";\n";
        } else {
            g << "    float " << var_name << " = 1e9;\n";
        }
    }
}

Result compile_to_glsl(const Scene& scene, std::string& out_glsl) {
    std::ostringstream g;
    g << std::fixed << std::setprecision(4);
    g << "#version 450\n";
    g << "// Generated by Mesh GPU Shader Compiler (meshc v" << VERSION << " --target glsl)\n";
    g << "// Scene: \"" << scene.name << "\" (" << scene.width << "x" << scene.height << ")\n\n";
    g << "layout(location = 0) in vec2 v_uv;\n";
    g << "layout(location = 0) out vec4 fragColor;\n";
    g << "uniform vec2 u_resolution;\n";
    g << "uniform float u_time;\n\n";

    g << R"GLSL(float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

float sdCircle(vec2 p, vec2 c, float r) {
    return length(p - c) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 q = p / ab;
    float r = length(q);
    float grad = length(p / (ab * ab));
    return (r - 1.0) * (r / max(grad, 1e-6));
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h);
}

float sminSdf(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / max(k, 1e-4), 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

void main() {
    vec2 p = v_uv * vec2()GLSL" << scene.width << ".0, " << scene.height << ".0);\n";

    Color bg = scene.has_background ? scene.background : Color::from_srgb(0.05, 0.07, 0.10, 1.0);
    g << "    vec4 col = vec4(" << bg.r << ", " << bg.g << ", " << bg.b << ", " << bg.a << ");\n";

    std::vector<const ShapeNode*> leaves;
    for (const auto& n : scene.nodes) collect_shader_leaves(n, leaves);

    int idx = 0;
    int sub_id = 0;
    for (const ShapeNode* leaf : leaves) {
        ++idx;
        std::string v_d = "d_" + std::to_string(idx);
        emit_glsl_shape_sdf(*leaf, v_d, sub_id, g);
        if (leaf->style.fill.kind != PaintKind::None && leaf->kind != ShapeKind::Line) {
            Color fc = leaf->style.fill.sample(leaf->bbox.center(), leaf->bbox);
            g << "    float cov_f_" << idx << " = clamp(0.5 - " << v_d << ", 0.0, 1.0) * " << (fc.a * leaf->style.opacity) << ";\n";
            g << "    col.rgb = mix(col.rgb, vec3(" << fc.r << ", " << fc.g << ", " << fc.b << "), cov_f_" << idx << ");\n";
        }
        if (leaf->style.stroke.kind != PaintKind::None && leaf->style.stroke_width > 0.0) {
            Color sc = leaf->style.stroke.sample(leaf->bbox.center(), leaf->bbox);
            g << "    float cov_s_" << idx << " = clamp(0.5 - (abs(" << v_d << ") - " << (leaf->style.stroke_width * 0.5) << "), 0.0, 1.0) * " << (sc.a * leaf->style.opacity) << ";\n";
            g << "    col.rgb = mix(col.rgb, vec3(" << sc.r << ", " << sc.g << ", " << sc.b << "), cov_s_" << idx << ");\n";
        }
    }

    g << "    fragColor = col;\n}\n";
    out_glsl = g.str();
    return {true, "", 0, 0};
}

// ============================================================================
// 4. Legacy SVG -> `.mesh` Importer (`meshc import <input.svg>`)
// ============================================================================

static std::string extract_xml_attr(const std::string& tag, const std::string& attr_name) {
    size_t pos = 0;
    while ((pos = tag.find(attr_name, pos)) != std::string::npos) {
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(tag[pos - 1])) || tag[pos - 1] == '-')) {
            pos += attr_name.size();
            continue;
        }
        size_t eq = pos + attr_name.size();
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) eq++;
        if (eq < tag.size() && tag[eq] == '=') {
            size_t q1 = eq + 1;
            while (q1 < tag.size() && std::isspace(static_cast<unsigned char>(tag[q1]))) q1++;
            if (q1 < tag.size() && (tag[q1] == '"' || tag[q1] == '\'')) {
                char q = tag[q1];
                size_t q2 = tag.find(q, q1 + 1);
                if (q2 != std::string::npos) {
                    return tag.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        pos += attr_name.size();
    }
    return "";
}

static std::vector<double> parse_svg_numbers(const std::string& s) {
    std::vector<double> nums;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '-' && s[i] != '+' && s[i] != '.') {
            ++i;
        }
        if (i >= s.size()) break;
        size_t start = i;
        if (s[i] == '-' || s[i] == '+') ++i;
        bool has_dot = false;
        while (i < s.size()) {
            if (std::isdigit(static_cast<unsigned char>(s[i]))) {
                ++i;
            } else if (s[i] == '.' && !has_dot) {
                has_dot = true;
                ++i;
            } else {
                break;
            }
        }
        if (i > start) {
            try { nums.push_back(std::stod(s.substr(start, i - start))); } catch (...) {}
        } else {
            ++i;
        }
    }
    return nums;
}

static void convert_svg_path_d(const std::string& d, std::ostringstream& out, const std::string& pad) {
    size_t i = 0;
    double cx = 0.0, cy = 0.0;
    double sx = 0.0, sy = 0.0;
    char cmd = 0;

    auto next_num = [&]() -> double {
        while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
        size_t start = i;
        if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
        bool dot = false;
        while (i < d.size()) {
            if (std::isdigit(static_cast<unsigned char>(d[i]))) ++i;
            else if (d[i] == '.' && !dot) { dot = true; ++i; }
            else break;
        }
        return (i > start) ? std::stod(d.substr(start, i - start)) : 0.0;
    };

    auto has_more_nums = [&]() -> bool {
        size_t k = i;
        while (k < d.size() && (std::isspace(static_cast<unsigned char>(d[k])) || d[k] == ',')) ++k;
        return k < d.size() && (std::isdigit(static_cast<unsigned char>(d[k])) || d[k] == '-' || d[k] == '+' || d[k] == '.');
    };

    while (i < d.size()) {
        while (i < d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i] == ',')) ++i;
        if (i >= d.size()) break;

        if (std::isalpha(static_cast<unsigned char>(d[i]))) {
            cmd = d[i++];
        } else if (cmd == 0) {
            break;
        }

        switch (cmd) {
            case 'M': case 'm': {
                double x = next_num();
                double y = next_num();
                if (cmd == 'm') { x += cx; y += cy; }
                cx = sx = x; cy = sy = y;
                out << pad << "move(" << cx << ", " << cy << ")\n";
                cmd = (cmd == 'm') ? 'l' : 'L';
                break;
            }
            case 'L': case 'l': {
                do {
                    double x = next_num();
                    double y = next_num();
                    if (cmd == 'l') { x += cx; y += cy; }
                    cx = x; cy = y;
                    out << pad << "line(" << cx << ", " << cy << ")\n";
                } while (has_more_nums());
                break;
            }
            case 'H': case 'h': {
                do {
                    double x = next_num();
                    cx = (cmd == 'h') ? (cx + x) : x;
                    out << pad << "line(" << cx << ", " << cy << ")\n";
                } while (has_more_nums());
                break;
            }
            case 'V': case 'v': {
                do {
                    double y = next_num();
                    cy = (cmd == 'v') ? (cy + y) : y;
                    out << pad << "line(" << cx << ", " << cy << ")\n";
                } while (has_more_nums());
                break;
            }
            case 'C': case 'c': {
                do {
                    double x1 = next_num(), y1 = next_num();
                    double x2 = next_num(), y2 = next_num();
                    double x = next_num(),  y = next_num();
                    if (cmd == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
                    cx = x; cy = y;
                    out << pad << "cubic((" << x1 << ", " << y1 << "), (" << x2 << ", " << y2 << "), (" << cx << ", " << cy << "))\n";
                } while (has_more_nums());
                break;
            }
            case 'Q': case 'q': {
                do {
                    double x1 = next_num(), y1 = next_num();
                    double x = next_num(),  y = next_num();
                    if (cmd == 'q') { x1 += cx; y1 += cy; x += cx; y += cy; }
                    cx = x; cy = y;
                    out << pad << "quad((" << x1 << ", " << y1 << "), (" << cx << ", " << cy << "))\n";
                } while (has_more_nums());
                break;
            }
            case 'A': case 'a': {
                do {
                    double rx = next_num();
                    (void)next_num();
                    (void)next_num();
                    bool large = (next_num() != 0.0);
                    bool sweep = (next_num() != 0.0);
                    double x = next_num(), y = next_num();
                    if (cmd == 'a') { x += cx; y += cy; }
                    cx = x; cy = y;
                    out << pad << "arc_to((" << cx << ", " << cy << "), r: " << rx
                        << ", cw: " << (sweep ? "true" : "false")
                        << ", large: " << (large ? "true" : "false") << ")\n";
                } while (has_more_nums());
                break;
            }
            case 'Z': case 'z': {
                cx = sx; cy = sy;
                out << pad << "close\n";
                cmd = 0;
                break;
            }
            default:
                ++i;
                break;
        }
    }
}

static std::string svg_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string svg_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string svg_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos) { out.push_back('&'); continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent == "nbsp") out.push_back(' ');
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) cp = std::stoul(ent.substr(2), nullptr, 16);
                else cp = std::stoul(ent.substr(1), nullptr, 10);
            } catch (...) { cp = 0; }
            if (cp > 0 && cp < 0x80) out.push_back(static_cast<char>(cp));
            else if (cp >= 0x80 && cp <= 0x10FFFF) {
                // UTF-8 encode
                if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

static std::string svg_strip_tags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    return svg_unescape(out);
}

static std::string svg_collapse_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    return svg_trim(out);
}

static std::map<std::string, std::string> svg_parse_decls(const std::string& body) {
    std::map<std::string, std::string> decls;
    std::string item;
    int depth = 0;
    char quote = 0;
    auto flush = [&]() {
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string key = svg_lower(svg_trim(item.substr(0, colon)));
            std::string val = svg_trim(item.substr(colon + 1));
            size_t bang = val.find("!important");
            if (bang != std::string::npos) val = svg_trim(val.substr(0, bang));
            if (!key.empty() && !val.empty()) decls[key] = val;
        }
        item.clear();
    };
    for (char c : body) {
        if (quote) {
            item.push_back(c);
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; item.push_back(c); continue; }
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        if (c == ';' && depth == 0) { flush(); continue; }
        item.push_back(c);
    }
    flush();
    return decls;
}

using SvgClassMap = std::map<std::string, std::map<std::string, std::string>>;

static SvgClassMap svg_parse_classes(const std::string& css) {
    SvgClassMap classes;
    size_t pos = 0;
    while ((pos = css.find('{', pos)) != std::string::npos) {
        size_t end = css.find('}', pos);
        if (end == std::string::npos) break;
        size_t sel_start = css.rfind('}', pos);
        sel_start = (sel_start == std::string::npos) ? 0 : sel_start + 1;
        std::string selector = svg_trim(css.substr(sel_start, pos - sel_start));
        std::string body = css.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (selector.empty() || selector[0] == '@') continue;
        auto decls = svg_parse_decls(body);
        std::istringstream sels(selector);
        std::string part;
        while (std::getline(sels, part, ',')) {
            part = svg_trim(part);
            if (part.empty()) continue;
            // Only simple `.class` selectors (possibly the last compound part).
            size_t cut = part.find_last_of(" >+~");
            if (cut != std::string::npos) part = svg_trim(part.substr(cut + 1));
            if (part.empty() || part[0] != '.') continue;
            std::string name = part.substr(1);
            size_t stop = name.find_first_of(" .:#[");
            if (stop != std::string::npos) name = name.substr(0, stop);
            if (name.empty()) continue;
            for (const auto& [k, v] : decls) classes[name][k] = v;
        }
    }
    return classes;
}

static std::string svg_prop(const std::string& tag, const SvgClassMap& classes, const std::string& name) {
    std::string key = svg_lower(name);
    std::string style = extract_xml_attr(tag, "style");
    if (!style.empty()) {
        auto decls = svg_parse_decls(style);
        auto it = decls.find(key);
        if (it != decls.end()) return it->second;
    }
    std::string cls = extract_xml_attr(tag, "class");
    if (!cls.empty()) {
        std::istringstream cs(cls);
        std::string cname;
        while (cs >> cname) {
            auto cit = classes.find(cname);
            if (cit == classes.end()) continue;
            auto dit = cit->second.find(key);
            if (dit != cit->second.end()) return dit->second;
        }
    }
    return extract_xml_attr(tag, name);
}

static std::string svg_first_family(const std::string& value) {
    size_t comma = value.find(',');
    std::string first = svg_trim(comma == std::string::npos ? value : value.substr(0, comma));
    if (first.size() >= 2 && ((first.front() == '"' && first.back() == '"') ||
                              (first.front() == '\'' && first.back() == '\''))) {
        first = first.substr(1, first.size() - 2);
    }
    return first;
}

static std::string svg_color_value(const std::string& value) {
    std::string v = svg_trim(value);
    std::string lv = svg_lower(v);
    if (lv.empty() || lv == "none" || lv == "transparent") return lv == "none" ? "none" : (lv == "transparent" ? "transparent" : "");
    if (!v.empty() && v[0] == '#') return v;
    if (lv.rfind("rgb(", 0) == 0 || lv.rfind("rgba(", 0) == 0) {
        auto nums = parse_svg_numbers(v);
        if (nums.size() >= 3) {
            int r = static_cast<int>(std::clamp(nums[0], 0.0, 255.0));
            int g = static_cast<int>(std::clamp(nums[1], 0.0, 255.0));
            int b = static_cast<int>(std::clamp(nums[2], 0.0, 255.0));
            char buf[16];
            if (nums.size() >= 4) std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b,
                                                static_cast<int>(std::clamp(nums[3], 0.0, 1.0) * 255.0));
            else std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
            return buf;
        }
    }
    static const std::map<std::string, std::string> named = {
        {"black", "#000000"}, {"white", "#ffffff"}, {"red", "#ff0000"}, {"green", "#008000"},
        {"blue", "#0000ff"}, {"yellow", "#ffff00"}, {"cyan", "#00ffff"}, {"magenta", "#ff00ff"},
        {"gray", "#808080"}, {"grey", "#808080"}, {"silver", "#c0c0c0"}, {"maroon", "#800000"},
        {"olive", "#808000"}, {"lime", "#00ff00"}, {"aqua", "#00ffff"}, {"teal", "#008080"},
        {"navy", "#000080"}, {"fuchsia", "#ff00ff"}, {"purple", "#800080"}, {"orange", "#ffa500"},
        {"pink", "#ffc0cb"}, {"brown", "#a52a2a"}, {"gold", "#ffd700"}, {"darkgray", "#a9a9a9"},
        {"darkgrey", "#a9a9a9"}, {"lightgray", "#d3d3d3"}, {"lightgrey", "#d3d3d3"},
        {"steelblue", "#4682b4"}, {"skyblue", "#87ceeb"}, {"tomato", "#ff6347"}
    };
    auto it = named.find(lv);
    if (it != named.end()) return it->second;
    return v;
}

static double svg_length_value(const std::string& value, double fallback) {
    std::string v = svg_trim(value);
    if (v.empty() || svg_lower(v) == "normal" || svg_lower(v) == "auto") return fallback;
    auto nums = parse_svg_numbers(v);
    if (nums.empty()) return fallback;
    double n = nums[0];
    std::string lv = svg_lower(v);
    if (lv.find("pt") != std::string::npos) n *= 4.0 / 3.0;
    else if (lv.find("em") != std::string::npos) n *= (fallback > 0.0 ? fallback : 16.0);
    else if (lv.find("%") != std::string::npos) n = fallback * n / 100.0;
    return n;
}

static double svg_font_weight_value(const std::string& value) {
    std::string v = svg_lower(svg_trim(value));
    if (v == "thin" || v == "hairline") return 100.0;
    if (v == "extralight" || v == "ultralight") return 200.0;
    if (v == "light") return 300.0;
    if (v == "normal" || v == "regular" || v == "book") return 400.0;
    if (v == "medium") return 500.0;
    if (v == "semibold" || v == "demibold") return 600.0;
    if (v == "bold") return 700.0;
    if (v == "extrabold" || v == "ultrabold") return 800.0;
    if (v == "black" || v == "heavy") return 900.0;
    auto nums = parse_svg_numbers(v);
    return nums.empty() ? 400.0 : std::clamp(nums[0], 1.0, 1000.0);
}

static double svg_font_stretch_value(const std::string& value) {
    std::string v = svg_lower(svg_trim(value));
    if (v == "ultracondensed") return 50.0;
    if (v == "extracondensed") return 62.5;
    if (v == "condensed" || v == "narrow") return 75.0;
    if (v == "semicondensed") return 87.5;
    if (v == "normal" || v == "auto") return 100.0;
    if (v == "semiexpanded") return 112.5;
    if (v == "expanded" || v == "wide") return 125.0;
    if (v == "extraexpanded") return 150.0;
    if (v == "ultraexpanded") return 200.0;
    auto nums = parse_svg_numbers(v);
    return nums.empty() ? 100.0 : nums[0];
}

static Transform2D parse_svg_transform(const std::string& value) {
    Transform2D result;
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && (std::isspace(static_cast<unsigned char>(value[i])) || value[i] == ',')) ++i;
        if (i >= value.size()) break;
        size_t name_start = i;
        while (i < value.size() && std::isalpha(static_cast<unsigned char>(value[i]))) ++i;
        std::string name = svg_lower(value.substr(name_start, i - name_start));
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (i >= value.size() || value[i] != '(') {
            while (i < value.size() && value[i] != ')') ++i;
            if (i < value.size()) ++i;
            continue;
        }
        size_t close = value.find(')', i + 1);
        if (close == std::string::npos) close = value.size();
        auto n = parse_svg_numbers(value.substr(i + 1, close - i - 1));
        i = close + 1;
        Transform2D t;
        if (name == "translate" && !n.empty()) {
            t = Transform2D::translate(n[0], n.size() > 1 ? n[1] : 0.0);
        } else if (name == "scale" && !n.empty()) {
            t = Transform2D::scale(n[0], n.size() > 1 ? n[1] : n[0]);
        } else if (name == "rotate" && !n.empty()) {
            Vec2 pivot = n.size() >= 3 ? Vec2{n[1], n[2]} : Vec2{0.0, 0.0};
            t = Transform2D::rotate_deg(n[0], pivot);
        } else if (name == "matrix" && n.size() >= 6) {
            t.a = n[0]; t.b = n[1]; t.c = n[2]; t.d = n[3]; t.tx = n[4]; t.ty = n[5];
        } else if (name == "skewx" && !n.empty()) {
            t.c = std::tan(n[0] * PI / 180.0);
        } else if (name == "skewy" && !n.empty()) {
            t.b = std::tan(n[0] * PI / 180.0);
        } else {
            continue;
        }
        result = result.multiply(t);
    }
    return result;
}

Result import_svg_to_mesh(const std::string& svg_content, std::string& out_mesh_source) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "// Auto-migrated from legacy SVG by meshc v" << VERSION << "\n";
    out << "mesh \"imported_svg\" {\n";

    uint32_t w = 800, h = 600;
    size_t svg_pos = svg_content.find("<svg");
    if (svg_pos != std::string::npos) {
        size_t end_tag = svg_content.find('>', svg_pos);
        std::string header = svg_content.substr(svg_pos, end_tag - svg_pos + 1);
        std::string vb = extract_xml_attr(header, "viewBox");
        if (!vb.empty()) {
            auto nums = parse_svg_numbers(vb);
            if (nums.size() >= 4) {
                w = static_cast<uint32_t>(std::max(1.0, nums[2]));
                h = static_cast<uint32_t>(std::max(1.0, nums[3]));
            }
        } else {
            std::string ws = extract_xml_attr(header, "width");
            std::string hs = extract_xml_attr(header, "height");
            if (!ws.empty()) w = static_cast<uint32_t>(std::max(1.0, std::stod(ws)));
            if (!hs.empty()) h = static_cast<uint32_t>(std::max(1.0, std::stod(hs)));
        }
    }

    // --- Collect embedded <style> blocks: CSS classes + @font-face. ---
    std::string css_all;
    {
        size_t sp = 0;
        while ((sp = svg_content.find("<style", sp)) != std::string::npos) {
            size_t open_end = svg_content.find('>', sp);
            if (open_end == std::string::npos) break;
            size_t close = svg_content.find("</style", open_end);
            if (close == std::string::npos) break;
            css_all += svg_content.substr(open_end + 1, close - open_end - 1);
            css_all += "\n";
            sp = close + 7;
        }
    }
    SvgClassMap classes = svg_parse_classes(css_all);

    struct ImportedFace {
        std::string family;
        std::string src;
        std::string id;
        bool embedded = false;
    };
    std::vector<ImportedFace> faces;
    {
        std::string css_lower = svg_lower(css_all);
        size_t fp = 0;
        int face_idx = 0;
        while ((fp = css_lower.find("@font-face", fp)) != std::string::npos) {
            size_t brace = css_all.find('{', fp);
            if (brace == std::string::npos) break;
            size_t close = css_all.find('}', brace);
            if (close == std::string::npos) break;
            auto decls = svg_parse_decls(css_all.substr(brace + 1, close - brace - 1));
            ImportedFace face;
            auto fit = decls.find("font-family");
            if (fit != decls.end()) face.family = svg_first_family(fit->second);
            auto sit = decls.find("src");
            if (sit != decls.end()) {
                const std::string& srcs = sit->second;
                size_t u = 0;
                bool saw_url = false;
                while ((u = srcs.find("url(", u)) != std::string::npos) {
                    size_t uend = srcs.find(')', u + 4);
                    if (uend == std::string::npos) break;
                    std::string url = svg_trim(srcs.substr(u + 4, uend - u - 4));
                    if (url.size() >= 2 && ((url.front() == '"' && url.back() == '"') ||
                                            (url.front() == '\'' && url.back() == '\''))) {
                        url = url.substr(1, url.size() - 2);
                    }
                    saw_url = true;
                    if (svg_lower(url).rfind("data:", 0) == 0) {
                        if (face.src.empty()) face.embedded = true;
                    } else if (face.src.empty()) {
                        face.src = url;
                        face.embedded = false;
                    }
                    u = uend + 1;
                }
                (void)saw_url;
            }
            if (!face.family.empty()) {
                face.id = "imported_" + std::to_string(++face_idx);
                faces.push_back(face);
            }
            fp = close + 1;
        }
    }

    out << "  canvas {\n    width: " << w << "\n    height: " << h << "\n  }\n\n";

    std::map<std::string, std::string> face_by_family;
    for (const auto& face : faces) {
        if (face.embedded) {
            out << "  // Skipped @font-face \"" << mesh_quote(face.family)
                << "\": embedded data: URIs are not imported\n\n";
            continue;
        }
        out << "  font \"" << mesh_quote(face.id) << "\" {\n"
            << "    family: \"" << mesh_quote(face.family) << "\"\n";
        if (!face.src.empty()) out << "    src: \"" << mesh_quote(face.src) << "\"\n";
        out << "  }\n\n";
        if (!face.src.empty()) face_by_family[face.family] = face.id;
    }

    struct TStyle {
        std::string family;
        double size = 16.0;
        double weight = 400.0;
        bool weight_set = false;
        int style = 0; // 0 normal, 1 italic, 2 oblique
        double stretch = 100.0;
        bool stretch_set = false;
        double letter = 0.0;
        double word = 0.0;
        int align = 0; // 0 left, 1 center, 2 right
        bool baseline = true;
        std::string fill;
        std::string stroke;
        std::string stroke_width;
        std::string opacity;
    };

    auto read_text_style = [&](const std::string& t, const TStyle* base) -> TStyle {
        TStyle s = base ? *base : TStyle{};
        std::string v;
        v = svg_prop(t, classes, "font-family");
        if (!v.empty()) s.family = svg_first_family(v);
        v = svg_prop(t, classes, "font-size");
        if (!v.empty()) s.size = std::max(0.1, svg_length_value(v, s.size));
        v = svg_prop(t, classes, "font-weight");
        if (!v.empty()) { s.weight = svg_font_weight_value(v); s.weight_set = true; }
        v = svg_lower(svg_prop(t, classes, "font-style"));
        if (!v.empty()) {
            if (v == "italic") s.style = 1;
            else if (v == "oblique") s.style = 2;
            else if (v == "normal") s.style = 0;
        }
        v = svg_prop(t, classes, "font-stretch");
        if (!v.empty()) { s.stretch = svg_font_stretch_value(v); s.stretch_set = true; }
        v = svg_prop(t, classes, "letter-spacing");
        if (!v.empty()) s.letter = svg_length_value(v, s.size);
        v = svg_prop(t, classes, "word-spacing");
        if (!v.empty()) s.word = svg_length_value(v, s.size);
        v = svg_lower(svg_prop(t, classes, "text-anchor"));
        if (v == "middle") s.align = 1;
        else if (v == "end") s.align = 2;
        else if (v == "start") s.align = 0;
        v = svg_lower(svg_prop(t, classes, "dominant-baseline"));
        if (v.empty()) v = svg_lower(svg_prop(t, classes, "alignment-baseline"));
        if (!v.empty()) s.baseline = !(v == "middle" || v == "central");
        v = svg_prop(t, classes, "fill");
        if (!v.empty()) s.fill = svg_color_value(v);
        v = svg_prop(t, classes, "stroke");
        if (!v.empty()) s.stroke = svg_color_value(v);
        v = svg_prop(t, classes, "stroke-width");
        if (!v.empty()) s.stroke_width = fmt_num(std::max(0.0, svg_length_value(v, 1.5)), 6);
        v = svg_prop(t, classes, "opacity");
        if (!v.empty()) s.opacity = fmt_num(std::clamp(svg_length_value(v, 1.0), 0.0, 1.0), 6);
        return s;
    };

    auto emit_text_node = [&](const std::string& text_id, const std::string& content,
                              double tx, double ty, const TStyle& s, const Transform2D& tf) {
        out << "  text \"" << text_id << "\" {\n";
        out << "    at: (" << fmt_num(tx, 6) << ", " << fmt_num(ty, 6) << ")\n";
        if (s.baseline) out << "    baseline: true\n";
        out << "    content: \"" << mesh_quote(content) << "\"\n";
        out << "    size: " << fmt_num(s.size, 6) << "\n";
        if (s.align == 1) out << "    align: center\n";
        else if (s.align == 2) out << "    align: right\n";
        if (s.weight_set || std::abs(s.weight - 400.0) > 1e-9) {
            out << "    weight: " << fmt_num(s.weight, 3) << "\n";
        }
        if (std::abs(s.letter) > 1e-9) out << "    letter_spacing: " << fmt_num(s.letter, 6) << "\n";
        if (std::abs(s.word) > 1e-9) out << "    word_spacing: " << fmt_num(s.word, 6) << "\n";
        auto fit = face_by_family.find(s.family);
        if (fit != face_by_family.end()) {
            out << "    font: @" << fit->second << "\n";
        } else if (!s.family.empty()) {
            out << "    font_family: \"" << mesh_quote(s.family) << "\"\n";
        }
        if (s.style == 1) out << "    font_style: italic\n";
        else if (s.style == 2) out << "    font_style: oblique\n";
        if (s.stretch_set && std::abs(s.stretch - 100.0) > 1e-9) {
            out << "    font_stretch: " << fmt_num(s.stretch, 3) << "\n";
        }
        if (!s.fill.empty()) {
            if (s.fill.find("url(") == std::string::npos) out << "    fill: " << s.fill << "\n";
            else out << "    // gradient fill references are not imported; using the default text fill\n";
        }
        if (!s.stroke.empty() && s.stroke.find("url(") == std::string::npos) {
            out << "    stroke: " << s.stroke << "\n";
            out << "    stroke_width: " << (s.stroke_width.empty() ? "1.5" : s.stroke_width) << "\n";
        }
        if (!s.opacity.empty()) out << "    opacity: " << s.opacity << "\n";
        if (!tf.is_identity()) {
            out << "    matrix: (" << fmt_num(tf.a, 6) << ", " << fmt_num(tf.b, 6) << ", "
                << fmt_num(tf.c, 6) << ", " << fmt_num(tf.d, 6) << ", "
                << fmt_num(tf.tx, 6) << ", " << fmt_num(tf.ty, 6) << ")\n";
        }
        out << "  }\n\n";
    };

    size_t pos = 0;
    int shape_idx = 1;
    std::vector<Transform2D> g_stack{Transform2D::identity()};

    while ((pos = svg_content.find('<', pos)) != std::string::npos) {
        size_t end = svg_content.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = svg_content.substr(pos + 1, end - pos - 1);
        pos = end + 1;

        if (tag.empty() || tag[0] == '!' || tag[0] == '?') continue;
        if (tag[0] == '/') {
            std::string close = svg_trim(tag.substr(1));
            if (close == "g" && g_stack.size() > 1) g_stack.pop_back();
            continue;
        }

        std::istringstream iss(tag);
        std::string elem;
        iss >> elem;
        bool self_closing = !tag.empty() && tag.back() == '/';

        std::string id = extract_xml_attr(tag, "id");
        if (id.empty()) id = elem + "_" + std::to_string(shape_idx++);

        Transform2D elem_tf = parse_svg_transform(extract_xml_attr(tag, "transform"));
        Transform2D world_tf = g_stack.back().multiply(elem_tf);

        if (elem == "g") {
            if (!self_closing) g_stack.push_back(world_tf);
            continue;
        }
        if (elem == "defs" || elem == "svg" || elem == "symbol" || elem == "clipPath" ||
            elem == "mask" || elem == "pattern" || elem == "style" || elem == "title" ||
            elem == "desc" || elem == "metadata") {
            continue;
        }

        auto emit_style = [&](const std::string& pad) {
            std::string fill = svg_prop(tag, classes, "fill");
            std::string stroke = svg_prop(tag, classes, "stroke");
            std::string sw = svg_prop(tag, classes, "stroke-width");
            std::string op = svg_prop(tag, classes, "opacity");
            std::string fill_c = fill.empty() ? "" : svg_color_value(fill);
            std::string stroke_c = stroke.empty() ? "" : svg_color_value(stroke);

            if (!fill_c.empty() && fill_c != "none" && fill_c.find("url(") == std::string::npos) {
                out << pad << "fill: " << fill_c << "\n";
            } else if (fill_c == "none") {
                out << pad << "fill: none\n";
            } else if (fill_c.empty() && stroke_c.empty()) {
                out << pad << "fill: #000000\n";
            }
            if (!stroke_c.empty() && stroke_c != "none" && stroke_c.find("url(") == std::string::npos) {
                out << pad << "stroke: " << stroke_c << "\n";
                out << pad << "stroke_width: " << (sw.empty() ? "1.5" : fmt_num(svg_length_value(sw, 1.5), 6)) << "\n";
            }
            if (!op.empty()) {
                out << pad << "opacity: " << fmt_num(std::clamp(svg_length_value(op, 1.0), 0.0, 1.0), 6) << "\n";
            }
            if (!world_tf.is_identity()) {
                out << pad << "matrix: (" << fmt_num(world_tf.a, 6) << ", " << fmt_num(world_tf.b, 6) << ", "
                    << fmt_num(world_tf.c, 6) << ", " << fmt_num(world_tf.d, 6) << ", "
                    << fmt_num(world_tf.tx, 6) << ", " << fmt_num(world_tf.ty, 6) << ")\n";
            }
        };

        if (elem == "rect") {
            std::string x = extract_xml_attr(tag, "x");
            std::string y = extract_xml_attr(tag, "y");
            std::string rw = extract_xml_attr(tag, "width");
            std::string rh = extract_xml_attr(tag, "height");
            std::string rx = extract_xml_attr(tag, "rx");
            if (rw.empty() || rh.empty()) continue;
            out << "  rect \"" << id << "\" {\n";
            out << "    x: " << (x.empty() ? "0" : fmt_num(svg_length_value(x, 0.0), 6)) << "\n";
            out << "    y: " << (y.empty() ? "0" : fmt_num(svg_length_value(y, 0.0), 6)) << "\n";
            out << "    width: " << fmt_num(svg_length_value(rw, 0.0), 6) << "\n";
            out << "    height: " << fmt_num(svg_length_value(rh, 0.0), 6) << "\n";
            if (!rx.empty()) out << "    radius: " << fmt_num(svg_length_value(rx, 0.0), 6) << "\n";
            emit_style("    ");
            out << "  }\n\n";
        } else if (elem == "circle") {
            std::string cx = extract_xml_attr(tag, "cx");
            std::string cy = extract_xml_attr(tag, "cy");
            std::string r = extract_xml_attr(tag, "r");
            if (r.empty()) continue;
            out << "  circle \"" << id << "\" {\n";
            out << "    center: (" << fmt_num(svg_length_value(cx, 0.0), 6) << ", "
                << fmt_num(svg_length_value(cy, 0.0), 6) << ")\n";
            out << "    radius: " << fmt_num(svg_length_value(r, 0.0), 6) << "\n";
            emit_style("    ");
            out << "  }\n\n";
        } else if (elem == "ellipse") {
            std::string cx = extract_xml_attr(tag, "cx");
            std::string cy = extract_xml_attr(tag, "cy");
            std::string rx = extract_xml_attr(tag, "rx");
            std::string ry = extract_xml_attr(tag, "ry");
            if (rx.empty() || ry.empty()) continue;
            out << "  ellipse \"" << id << "\" {\n";
            out << "    center: (" << fmt_num(svg_length_value(cx, 0.0), 6) << ", "
                << fmt_num(svg_length_value(cy, 0.0), 6) << ")\n";
            out << "    rx: " << fmt_num(svg_length_value(rx, 0.0), 6) << "\n";
            out << "    ry: " << fmt_num(svg_length_value(ry, 0.0), 6) << "\n";
            emit_style("    ");
            out << "  }\n\n";
        } else if (elem == "line") {
            std::string x1 = extract_xml_attr(tag, "x1");
            std::string y1 = extract_xml_attr(tag, "y1");
            std::string x2 = extract_xml_attr(tag, "x2");
            std::string y2 = extract_xml_attr(tag, "y2");
            out << "  line \"" << id << "\" {\n";
            out << "    from: (" << fmt_num(svg_length_value(x1, 0.0), 6) << ", "
                << fmt_num(svg_length_value(y1, 0.0), 6) << ")\n";
            out << "    to: (" << fmt_num(svg_length_value(x2, 0.0), 6) << ", "
                << fmt_num(svg_length_value(y2, 0.0), 6) << ")\n";
            emit_style("    ");
            out << "  }\n\n";
        } else if (elem == "polygon" || elem == "polyline") {
            std::string pts_str = extract_xml_attr(tag, "points");
            auto nums = parse_svg_numbers(pts_str);
            if (nums.size() >= 4) {
                out << "  " << elem << " \"" << id << "\" {\n    points: [";
                for (size_t k = 0; k + 1 < nums.size(); k += 2) {
                    if (k > 0) out << ", ";
                    out << "(" << nums[k] << ", " << nums[k + 1] << ")";
                }
                out << "]\n";
                emit_style("    ");
                out << "  }\n\n";
            }
        } else if (elem == "path") {
            std::string d = extract_xml_attr(tag, "d");
            if (!d.empty()) {
                out << "  path \"" << id << "\" {\n";
                convert_svg_path_d(d, out, "    ");
                emit_style("    ");
                out << "  }\n\n";
            }
        } else if (elem == "text") {
            TStyle base_style = read_text_style(tag, nullptr);
            double bx = svg_length_value(extract_xml_attr(tag, "x"), 0.0);
            double by = svg_length_value(extract_xml_attr(tag, "y"), 0.0);

            size_t close_text = svg_content.find("</text", pos);
            std::string inner;
            if (close_text == std::string::npos) {
                inner = svg_content.substr(pos);
                pos = svg_content.size();
            } else {
                inner = svg_content.substr(pos, close_text - pos);
                size_t inner_end = svg_content.find('>', close_text);
                pos = (inner_end == std::string::npos) ? svg_content.size() : inner_end + 1;
            }

            struct TextRun {
                std::string tag;
                std::string raw;
            };
            std::vector<TextRun> runs;
            std::string inner_lower = svg_lower(inner);
            size_t tp = 0;
            while ((tp = inner_lower.find("<tspan", tp)) != std::string::npos) {
                size_t te = inner.find('>', tp);
                if (te == std::string::npos) break;
                size_t cs = te + 1;
                size_t ce = inner.find('<', cs);
                if (ce == std::string::npos) ce = inner.size();
                runs.push_back({svg_trim(inner.substr(tp + 1, te - tp - 1)),
                                inner.substr(cs, ce - cs)});
                tp = ce;
            }

            bool positioned = false;
            for (const auto& run : runs) {
                if (!extract_xml_attr(run.tag, "x").empty() || !extract_xml_attr(run.tag, "y").empty() ||
                    !extract_xml_attr(run.tag, "dx").empty() || !extract_xml_attr(run.tag, "dy").empty()) {
                    positioned = true;
                    break;
                }
            }

            if (runs.empty()) {
                std::string content = svg_collapse_ws(svg_strip_tags(inner));
                if (!content.empty()) emit_text_node(id, content, bx, by, base_style, world_tf);
            } else if (!positioned) {
                // No per-tspan placement: the tspans are inline runs of one line,
                // so keep text that appears outside the tspans as well.
                std::string combined = svg_collapse_ws(svg_strip_tags(inner));
                if (!combined.empty()) emit_text_node(id, combined, bx, by, base_style, world_tf);
            } else {
                int run_idx = 0;
                for (const auto& run : runs) {
                    std::string content = svg_collapse_ws(svg_strip_tags(run.raw));
                    if (content.empty()) continue;
                    TStyle rs = read_text_style(run.tag, &base_style);
                    double rx = extract_xml_attr(run.tag, "x").empty()
                                    ? bx
                                    : svg_length_value(extract_xml_attr(run.tag, "x"), bx);
                    double ry = extract_xml_attr(run.tag, "y").empty()
                                    ? by
                                    : svg_length_value(extract_xml_attr(run.tag, "y"), by);
                    rx += svg_length_value(extract_xml_attr(run.tag, "dx"), 0.0);
                    ry += svg_length_value(extract_xml_attr(run.tag, "dy"), 0.0);
                    Transform2D run_tf = world_tf.multiply(
                        parse_svg_transform(extract_xml_attr(run.tag, "transform")));
                    std::string run_id = (runs.size() > 1) ? (id + "_" + std::to_string(++run_idx)) : id;
                    emit_text_node(run_id, content, rx, ry, rs, run_tf);
                }
            }
        }
    }

    out << "}\n";
    out_mesh_source = out.str();
    return {true, "", 0, 0};
}

// ============================================================================
// 5. Backwards-Compatible SVG 1.1 Compiler (`meshc compile --target svg`)
// ============================================================================

static std::string render_single_node_data_uri(const Scene& parent_scene, const ShapeNode& node) {
    Scene mini;
    mini.width = parent_scene.width;
    mini.height = parent_scene.height;
    mini.has_background = false;
    mini.named_nodes = parent_scene.named_nodes;
    mini.paint_servers = parent_scene.paint_servers;
    mini.nodes.push_back(node);

    RasterImage img;
    if (!render_scene(mini, img).ok) return "";
    std::vector<uint8_t> png;
    if (!encode_png(img, png).ok) return "";
    return "data:image/png;base64," + base64_encode(png);
}

static void emit_svg_node(const Scene& scene, const ShapeNode& node, std::ostringstream& svg, int indent) {
    std::string pad(indent * 2, ' ');

    if (node.kind == ShapeKind::Group) {
        svg << pad << "<g";
        if (!node.id.empty()) svg << " id=\"" << xml_escape(node.id) << "\"";
        svg << ">\n";
        for (const auto& ch : node.children) {
            emit_svg_node(scene, ch, svg, indent + 1);
        }
        svg << pad << "</g>\n";
        return;
    }

    bool needs_raster_bake = (node.kind == ShapeKind::Boolean) ||
                             (node.style.fill.kind == PaintKind::DiffusionGradient) ||
                             (node.style.fill.kind == PaintKind::MeshGradient) ||
                             (node.style.fill.kind == PaintKind::ConicGradient) ||
                             (node.style.fill.kind == PaintKind::NoiseGradient) ||
                             (!node.style.clip_id.empty()) ||
                             (node.style.blend_mode != BlendMode::Normal) ||
                             (node.style.arrow_mode != ArrowMode::None) ||
                             (node.style.stroke_align != StrokeAlign::Center && node.style.stroke_width > 0.0) ||
                             (node.kind == ShapeKind::Text &&
                              (node.style.shadow.enabled || node.style.glow.enabled || node.style.grain > 0.0));

    if (needs_raster_bake) {
        std::string uri = render_single_node_data_uri(scene, node);
        if (!uri.empty()) {
            svg << pad << "<image";
            if (!node.id.empty()) svg << " id=\"" << xml_escape(node.id) << "\"";
            svg << " x=\"0\" y=\"0\" width=\"" << scene.width << "\" height=\"" << scene.height
                << "\" href=\"" << uri << "\"/>\n";
            return;
        }
    }

    auto style_attrs = [&]() {
        std::ostringstream ss;
        if (!node.id.empty()) ss << " id=\"" << xml_escape(node.id) << "\"";

        if (node.style.fill.kind == PaintKind::None) {
            ss << " fill=\"none\"";
        } else if (node.style.fill.kind == PaintKind::Solid) {
            ss << " fill=\"" << node.style.fill.solid_color.to_hex() << "\"";
            if (node.style.fill.solid_color.a < 0.999) {
                ss << " fill-opacity=\"" << std::fixed << std::setprecision(3) << node.style.fill.solid_color.a << "\"";
            }
        } else if (!node.style.fill.id.empty()) {
            ss << " fill=\"url(#" << xml_escape(node.style.fill.id) << ")\"";
        }

        if (node.style.stroke.kind != PaintKind::None && node.style.stroke_width > 0.0) {
            if (node.style.stroke.kind == PaintKind::Solid) {
                ss << " stroke=\"" << node.style.stroke.solid_color.to_hex() << "\"";
                if (node.style.stroke.solid_color.a < 0.999) {
                    ss << " stroke-opacity=\"" << std::fixed << std::setprecision(3) << node.style.stroke.solid_color.a << "\"";
                }
            } else if (!node.style.stroke.id.empty()) {
                ss << " stroke=\"url(#" << xml_escape(node.style.stroke.id) << ")\"";
            }
            ss << " stroke-width=\"" << node.style.stroke_width << "\"";
            if (node.style.line_cap == LineCap::Round) ss << " stroke-linecap=\"round\"";
            else if (node.style.line_cap == LineCap::Square) ss << " stroke-linecap=\"square\"";
            if (node.style.line_join == LineJoin::Round) ss << " stroke-linejoin=\"round\"";
            else if (node.style.line_join == LineJoin::Bevel) ss << " stroke-linejoin=\"bevel\"";
            if (!node.style.dash_array.empty()) {
                ss << " stroke-dasharray=\"";
                for (size_t i = 0; i < node.style.dash_array.size(); ++i) {
                    if (i > 0) ss << ",";
                    ss << node.style.dash_array[i];
                }
                ss << "\"";
            }
        }

        if (node.style.opacity < 0.999) {
            ss << " opacity=\"" << std::fixed << std::setprecision(3) << node.style.opacity << "\"";
        }

        const auto& m = node.transform;
        if (std::abs(m.a - 1.0) > 1e-6 || std::abs(m.b) > 1e-6 || std::abs(m.c) > 1e-6 ||
            std::abs(m.d - 1.0) > 1e-6 || std::abs(m.tx) > 1e-6 || std::abs(m.ty) > 1e-6) {
            ss << " transform=\"matrix(" << m.a << " " << m.b << " " << m.c << " " << m.d << " " << m.tx << " " << m.ty << ")\"";
        }
        return ss.str();
    };

    switch (node.kind) {
        case ShapeKind::Rect: {
            svg << pad << "<rect x=\"" << node.x << "\" y=\"" << node.y
                << "\" width=\"" << node.width << "\" height=\"" << node.height << "\"";
            if (node.corner_radius > 0.0) {
                svg << " rx=\"" << node.corner_radius << "\" ry=\"" << node.corner_radius << "\"";
            }
            svg << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Circle: {
            svg << pad << "<circle cx=\"" << node.center.x << "\" cy=\"" << node.center.y
                << "\" r=\"" << node.radius << "\"" << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Ellipse: {
            svg << pad << "<ellipse cx=\"" << node.center.x << "\" cy=\"" << node.center.y
                << "\" rx=\"" << node.rx << "\" ry=\"" << node.ry << "\"" << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Line: {
            svg << pad << "<line x1=\"" << node.from.x << "\" y1=\"" << node.from.y
                << "\" x2=\"" << node.to.x << "\" y2=\"" << node.to.y << "\"" << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Polygon:
        case ShapeKind::Polyline: {
            std::string tag = (node.kind == ShapeKind::Polygon) ? "polygon" : "polyline";
            svg << pad << "<" << tag << " points=\"";
            for (size_t i = 0; i < node.points.size(); ++i) {
                if (i > 0) svg << " ";
                svg << node.points[i].x << "," << node.points[i].y;
            }
            svg << "\"" << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Spline:
        case ShapeKind::Path: {
            svg << pad << "<path d=\"";
            for (size_t i = 0; i < node.path_segments.size(); ++i) {
                if (i > 0) svg << " ";
                const auto& s = node.path_segments[i];
                switch (s.verb) {
                    case PathVerb::MoveTo:
                        svg << "M " << s.p1.x << " " << s.p1.y;
                        break;
                    case PathVerb::LineTo:
                        svg << "L " << s.p1.x << " " << s.p1.y;
                        break;
                    case PathVerb::QuadTo:
                        svg << "Q " << s.p1.x << " " << s.p1.y << " " << s.p2.x << " " << s.p2.y;
                        break;
                    case PathVerb::CubicTo:
                        svg << "C " << s.p1.x << " " << s.p1.y << " " << s.p2.x << " " << s.p2.y
                            << " " << s.p3.x << " " << s.p3.y;
                        break;
                    case PathVerb::ArcTo:
                        svg << "A " << s.radius << " " << s.radius << " 0 "
                            << (s.large_arc ? 1 : 0) << " " << (s.clockwise ? 1 : 0) << " "
                            << s.p1.x << " " << s.p1.y;
                        break;
                    case PathVerb::BulgeTo: {
                        for (const auto& sp : node.subpaths) {
                            for (size_t k = 1; k < sp.points.size(); ++k) {
                                svg << " L " << sp.points[k].x << " " << sp.points[k].y;
                            }
                        }
                        break;
                    }
                    case PathVerb::Close:
                        svg << "Z";
                        break;
                }
            }
            svg << "\"" << style_attrs() << "/>\n";
            break;
        }
        case ShapeKind::Text: {
            std::string family = !node.resolved_font_family.empty()
                                     ? node.resolved_font_family
                                     : (!node.font_family.empty() ? node.font_family : scene.default_font_family);

            // Preferred: exact shaped outlines in local node space. The path is
            // filled/stroked by the same style attributes as any other node, so
            // gradients, transforms and opacity survive verbatim.
            if (!node.text_subpaths.empty()) {
                svg << pad << "<path d=\"";
                for (const auto& sp : node.text_subpaths) {
                    if (sp.points.empty()) continue;
                    svg << "M " << sp.points[0].x << " " << sp.points[0].y;
                    for (size_t i = 1; i < sp.points.size(); ++i) {
                        svg << " L " << sp.points[i].x << " " << sp.points[i].y;
                    }
                    if (sp.closed) svg << " Z";
                    svg << " ";
                }
                svg << "\"" << style_attrs();
                svg << " data-mesh-text=\"" << xml_escape(node.text_content) << "\"";
                svg << " data-mesh-font=\"" << xml_escape(family) << "\"/>\n";
                break;
            }

            std::string anchor = "start";
            if (node.text_align == TextAlign::Center) anchor = "middle";
            else if (node.text_align == TextAlign::Right) anchor = "end";

            // `baseline_anchor=false` positions the visual centre; approximate
            // the SVG baseline from the shaped ascent/descent when available.
            double baseline_y = node.text_pos.y;
            if (!node.baseline_anchor) {
                if (node.text_ascent > 0.0 || node.text_descent > 0.0) {
                    baseline_y += (node.text_ascent - node.text_descent) * 0.5;
                } else {
                    baseline_y += node.font_size * 0.35;
                }
            }
            std::ostringstream fam_attr;
            fam_attr << family;
            if (!node.font_fallback.empty() && node.font_fallback != family) {
                fam_attr << ", " << node.font_fallback;
            } else if (!scene.default_font_fallback.empty() && scene.default_font_fallback != family) {
                fam_attr << ", " << scene.default_font_fallback;
            }

            svg << pad << "<text x=\"" << node.text_pos.x << "\" y=\"" << baseline_y
                << "\" font-family=\"" << xml_escape(fam_attr.str()) << "\""
                << " font-weight=\"" << static_cast<int>(node.font_weight) << "\" font-size=\""
                << node.font_size << "\"";
            if (node.font_style == FontStyle::Italic) svg << " font-style=\"italic\"";
            else if (node.font_style == FontStyle::Oblique) svg << " font-style=\"oblique\"";
            if (std::abs(node.font_stretch - 100.0) > 1e-9) {
                svg << " font-stretch=\"" << node.font_stretch << "%\"";
            }
            if (std::abs(node.letter_spacing) > 1e-9) svg << " letter-spacing=\"" << node.letter_spacing << "\"";
            if (std::abs(node.word_spacing) > 1e-9) svg << " word-spacing=\"" << node.word_spacing << "\"";
            svg << " text-anchor=\"" << anchor << "\" xml:space=\"preserve\"" << style_attrs()
                << " data-mesh-text=\"" << xml_escape(node.text_content) << "\""
                << " data-mesh-font=\"" << xml_escape(family) << "\">"
                << xml_escape(node.text_content) << "</text>\n";
            break;
        }
        default:
            break;
    }
}

Result compile_to_svg(const Scene& scene, std::string& out_svg) {
    std::ostringstream svg;
    svg << std::fixed << std::setprecision(2);
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<!-- Generated by Mesh Compiler (meshc v" << VERSION << ") -->\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << scene.width << " " << scene.height
        << "\" width=\"" << scene.width << "\" height=\"" << scene.height << "\">\n";

    if (!scene.paint_servers.empty()) {
        svg << "  <defs>\n";
        for (const auto& [id, ps] : scene.paint_servers) {
            if (ps.kind == PaintKind::LinearGradient) {
                svg << "    <linearGradient id=\"" << xml_escape(id)
                    << "\" x1=\"" << ps.start.x << "\" y1=\"" << ps.start.y
                    << "\" x2=\"" << ps.end.x << "\" y2=\"" << ps.end.y << "\"";
                if (ps.user_space) svg << " gradientUnits=\"userSpaceOnUse\"";
                svg << ">\n";
                if (ps.stops.size() >= 2) {
                    const int samples = 12;
                    for (int i = 0; i <= samples; ++i) {
                        double t = static_cast<double>(i) / samples;
                        BBox dummy{0, 0, 1, 1, true};
                        Vec2 pt = ps.start + (ps.end - ps.start) * t;
                        Color c = ps.sample(pt, dummy);
                        svg << "      <stop offset=\"" << (t * 100.0) << "%\" stop-color=\"" << c.to_hex()
                            << "\" stop-opacity=\"" << c.a << "\"/>\n";
                    }
                }
                svg << "    </linearGradient>\n";
            } else if (ps.kind == PaintKind::RadialGradient) {
                svg << "    <radialGradient id=\"" << xml_escape(id)
                    << "\" cx=\"" << ps.center.x << "\" cy=\"" << ps.center.y
                    << "\" r=\"" << ps.radius << "\"";
                if (ps.user_space) svg << " gradientUnits=\"userSpaceOnUse\"";
                svg << ">\n";
                for (const auto& st : ps.stops) {
                    svg << "      <stop offset=\"" << (st.offset * 100.0) << "%\" stop-color=\"" << st.color.to_hex()
                        << "\" stop-opacity=\"" << st.color.a << "\"/>\n";
                }
                svg << "    </radialGradient>\n";
            }
        }
        svg << "  </defs>\n";
    }

    if (scene.has_background) {
        svg << "  <rect width=\"" << scene.width << "\" height=\"" << scene.height
            << "\" fill=\"" << scene.background.to_hex() << "\" fill-opacity=\"" << scene.background.a << "\"/>\n";
    }

    for (const auto& node : scene.nodes) {
        emit_svg_node(scene, node, svg, 1);
    }

    svg << "</svg>\n";
    out_svg = svg.str();
    return {true, "", 0, 0};
}

// ============================================================================
// 6. Interactive WebGPU WGSL + HTML5 Canvas Runtime (`meshc compile --target html`)
// ============================================================================

Result compile_to_html(const Scene& scene, const std::string& original_source, std::string& out_html) {
    RasterImage img;
    Result rr = render_scene(scene, img);
    if (!rr.ok) return rr;

    std::vector<uint8_t> png_bytes;
    Result er = encode_png(img, png_bytes);
    if (!er.ok) return er;

    std::string png_b64 = base64_encode(png_bytes);
    std::string wgsl_code;
    compile_to_wgsl(scene, wgsl_code);

    std::ostringstream bboxes_json;
    bboxes_json << std::fixed << std::setprecision(2) << "[\n";
    bool first = true;
    for (const auto& [id, bb] : scene.node_bboxes) {
        if (!bb.valid) continue;
        if (!first) bboxes_json << ",\n";
        first = false;
        bboxes_json << "  {\"id\":\"" << xml_escape(id) << "\",\"x\":" << bb.min_x << ",\"y\":" << bb.min_y
                    << ",\"w\":" << bb.width() << ",\"h\":" << bb.height()
                    << ",\"cx\":" << bb.center_x() << ",\"cy\":" << bb.center_y() << "}";
    }
    bboxes_json << "\n]";

    std::ostringstream html;
    html << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>.mesh Browser Runtime v1.0.0 — )HTML" << xml_escape(scene.name) << R"HTML(</title>
<style>
  :root {
    --bg: #0b0e14;
    --panel: #131822;
    --border: #252e42;
    --text: #e6edf3;
    --muted: #8b949e;
    --accent: #58a6ff;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    height: 100vh;
    display: grid;
    grid-template-rows: 52px 1fr;
    overflow: hidden;
  }
  header {
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 20px;
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 12px;
    font-weight: 700;
    font-size: 15px;
    letter-spacing: 0.4px;
  }
  .badge {
    background: rgba(88, 166, 255, 0.15);
    color: var(--accent);
    border: 1px solid rgba(88, 166, 255, 0.35);
    padding: 3px 8px;
    border-radius: 6px;
    font-size: 12px;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  button {
    background: #1c2433;
    color: var(--text);
    border: 1px solid var(--border);
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 13px;
    cursor: pointer;
    transition: all 0.15s ease;
  }
  button:hover { border-color: var(--accent); }
  button.active { background: rgba(88, 166, 255, 0.2); border-color: var(--accent); color: #fff; }
  main {
    display: grid;
    grid-template-columns: 1fr 440px;
    height: 100%;
    overflow: hidden;
  }
  .viewport {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    background-image: radial-gradient(#1f2937 1px, transparent 1px);
    background-size: 20px 20px;
    overflow: hidden;
    cursor: grab;
  }
  .viewport:active { cursor: grabbing; }
  canvas {
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.65);
    border-radius: 4px;
  }
  .sidebar {
    background: var(--panel);
    border-left: 1px solid var(--border);
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }
  .sidebar-tabs {
    display: flex;
    border-bottom: 1px solid var(--border);
  }
  .sidebar-tab {
    flex: 1;
    text-align: center;
    padding: 10px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
    color: var(--muted);
    background: transparent;
    border: none;
    border-radius: 0;
  }
  .sidebar-tab.active {
    color: var(--accent);
    border-bottom: 2px solid var(--accent);
    background: rgba(88, 166, 255, 0.08);
  }
  pre {
    flex: 1;
    padding: 16px;
    overflow: auto;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 12px;
    line-height: 1.5;
    color: #d2a8ff;
    background: #0d1117;
  }
  .hud {
    position: absolute;
    bottom: 16px;
    left: 16px;
    background: rgba(19, 24, 34, 0.88);
    border: 1px solid var(--border);
    padding: 8px 12px;
    border-radius: 6px;
    font-size: 12px;
    font-family: ui-monospace, monospace;
    color: var(--muted);
    pointer-events: none;
  }
</style>
</head>
<body>
<header>
  <div class="brand">
    <span>MESH ENGINE v1.0.0</span>
    <span class="badge">)HTML" << xml_escape(scene.name) << R"HTML(.mesh</span>
    <span class="badge">)HTML" << scene.width << "x" << scene.height << R"HTML(</span>
    <span class="badge" id="gpuStatus">Canvas2D + WebGPU WGSL</span>
  </div>
  <div class="controls">
    <button id="btnAnchors" onclick="toggleAnchors()">@id Anchors Inspector</button>
    <button onclick="resetView()">Reset Zoom (100%)</button>
  </div>
</header>
<main>
  <div class="viewport" id="viewport">
    <canvas id="meshCanvas" width=")HTML" << scene.width << R"HTML(" height=")HTML" << scene.height << R"HTML("></canvas>
    <div class="hud" id="hud">Zoom: 100% | Pan: (0, 0) | Nodes: )HTML" << scene.node_bboxes.size() << R"HTML(</div>
  </div>
  <aside class="sidebar">
    <div class="sidebar-tabs">
      <button class="sidebar-tab active" id="tabMesh" onclick="switchTab('mesh')">.mesh Source</button>
      <button class="sidebar-tab" id="tabWgsl" onclick="switchTab('wgsl')">Compiled .wgsl Shader</button>
    </div>
    <pre id="codeMesh">)HTML" << xml_escape(original_source) << R"HTML(</pre>
    <pre id="codeWgsl" style="display:none; color:#7ee787;">)HTML" << xml_escape(wgsl_code) << R"HTML(</pre>
  </aside>
</main>
<script>
const SCENE_WIDTH = )HTML" << scene.width << R"HTML(;
const SCENE_HEIGHT = )HTML" << scene.height << R"HTML(;
const NODE_ANCHORS = )HTML" << bboxes_json.str() << R"HTML(;
const RASTER_URI = "data:image/png;base64,)HTML" << png_b64 << R"HTML(";

if (navigator.gpu) {
  document.getElementById('gpuStatus').textContent = 'WebGPU WGSL Ready';
}

function switchTab(which) {
  document.getElementById('codeMesh').style.display = (which === 'mesh') ? 'block' : 'none';
  document.getElementById('codeWgsl').style.display = (which === 'wgsl') ? 'block' : 'none';
  document.getElementById('tabMesh').classList.toggle('active', which === 'mesh');
  document.getElementById('tabWgsl').classList.toggle('active', which === 'wgsl');
}

const canvas = document.getElementById('meshCanvas');
const ctx = canvas.getContext('2d');
const viewport = document.getElementById('viewport');
const hud = document.getElementById('hud');

let zoom = 1.0;
let panX = 0;
let panY = 0;
let showAnchors = false;
let isDragging = false;
let dragStartX = 0, dragStartY = 0;

const baseImg = new Image();
baseImg.onload = () => draw();
baseImg.src = RASTER_URI;

function draw() {
  canvas.style.transform = `translate(${panX}px, ${panY}px) scale(${zoom})`;
  ctx.clearRect(0, 0, SCENE_WIDTH, SCENE_HEIGHT);
  ctx.drawImage(baseImg, 0, 0, SCENE_WIDTH, SCENE_HEIGHT);

  if (showAnchors) {
    ctx.save();
    for (const b of NODE_ANCHORS) {
      ctx.strokeStyle = '#38bdf8';
      ctx.lineWidth = 1.5;
      ctx.setLineDash([5, 4]);
      ctx.strokeRect(b.x, b.y, b.w, b.h);
      ctx.setLineDash([]);

      const pts = [[b.cx, b.cy], [b.x, b.y], [b.x + b.w, b.y], [b.x, b.y + b.h], [b.x + b.w, b.y + b.h]];
      for (const [px, py] of pts) {
        ctx.fillStyle = '#f43f5e';
        ctx.beginPath();
        ctx.arc(px, py, 3.5, 0, Math.PI * 2);
        ctx.fill();
      }

      ctx.fillStyle = 'rgba(15, 23, 42, 0.88)';
      const tag = `@${b.id} (${Math.round(b.w)}x${Math.round(b.h)})`;
      ctx.font = '11px ui-monospace, monospace';
      const tw = ctx.measureText(tag).width + 10;
      ctx.fillRect(b.x, Math.max(0, b.y - 20), tw, 18);
      ctx.fillStyle = '#38bdf8';
      ctx.fillText(tag, b.x + 5, Math.max(13, b.y - 7));
    }
    ctx.restore();
  }
  hud.textContent = `Zoom: ${Math.round(zoom * 100)}% | Pan: (${Math.round(panX)}, ${Math.round(panY)}) | Relational Nodes: ${NODE_ANCHORS.length}`;
}

function toggleAnchors() {
  showAnchors = !showAnchors;
  document.getElementById('btnAnchors').classList.toggle('active', showAnchors);
  draw();
}

function resetView() {
  zoom = 1.0;
  panX = 0;
  panY = 0;
  draw();
}

viewport.addEventListener('wheel', (e) => {
  e.preventDefault();
  const factor = e.deltaY < 0 ? 1.12 : 0.89;
  zoom = Math.min(8.0, Math.max(0.2, zoom * factor));
  draw();
}, { passive: false });

viewport.addEventListener('mousedown', (e) => {
  isDragging = true;
  dragStartX = e.clientX - panX;
  dragStartY = e.clientY - panY;
});

window.addEventListener('mousemove', (e) => {
  if (!isDragging) return;
  panX = e.clientX - dragStartX;
  panY = e.clientY - dragStartY;
  draw();
});

window.addEventListener('mouseup', () => { isDragging = false; });
</script>
</body>
</html>
)HTML";

    out_html = html.str();
    return {true, "", 0, 0};
}

// ============================================================================
// 7. Canonical Source Code Formatter (`meshc fmt`)
// ============================================================================

Result format_mesh_source(const std::string& source, std::string& out_formatted,
                          const std::string& base_dir) {
    Scene check_scene;
    Result r = parse_and_evaluate(source, check_scene, 0.0, base_dir);
    if (!r.ok) return r;

    std::ostringstream oss;
    int indent = 0;
    std::istringstream iss(source);
    std::string line;

    while (std::getline(iss, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            oss << "\n";
            continue;
        }
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, last - first + 1);

        if (!trimmed.empty() && trimmed[0] == '}') {
            indent = std::max(0, indent - 1);
        }

        oss << std::string(indent * 2, ' ') << trimmed << "\n";

        if (!trimmed.empty() && trimmed.back() == '{') {
            indent++;
        }
    }
    out_formatted = oss.str();
    return {true, "", 0, 0};
}

} // namespace mesh

// ============================================================================
// 8. C99 FFI Implementation (`include/mesh_c_api.h`)
// ============================================================================

static char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) {
        std::memcpy(p, s.c_str(), s.size() + 1);
    }
    return p;
}

extern "C" {

const char* mesh_version(void) {
    return mesh::VERSION;
}

mesh_status_t mesh_render_rgba(
    const char* mesh_source,
    uint32_t override_width,
    uint32_t override_height,
    mesh_image_buffer_t* out_buffer
) {
    return mesh_render_rgba_at_time(mesh_source, 0.0, override_width, override_height, 1, 0, out_buffer);
}

mesh_status_t mesh_render_rgba_at_time(
    const char* mesh_source,
    double time_sec,
    uint32_t override_width,
    uint32_t override_height,
    int ssaa_samples,
    int num_threads,
    mesh_image_buffer_t* out_buffer
) {
    if (!mesh_source || !out_buffer) return MESH_ERR_INVALID_ARG;
    std::memset(out_buffer, 0, sizeof(*out_buffer));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene, time_sec);
    if (!pr.ok) {
        out_buffer->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    mesh::RenderOptions opts;
    opts.override_width = override_width;
    opts.override_height = override_height;
    opts.ssaa_samples = ssaa_samples;
    opts.num_threads = num_threads;

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene_ex(scene, img, opts);
    if (!rr.ok) {
        out_buffer->error_message = dup_cstr(rr.error);
        return MESH_ERR_EVAL;
    }

    size_t n = img.rgba.size();
    out_buffer->rgba_pixels = static_cast<uint8_t*>(std::malloc(n));
    if (!out_buffer->rgba_pixels) return MESH_ERR_EVAL;
    std::memcpy(out_buffer->rgba_pixels, img.rgba.data(), n);
    out_buffer->width = img.width;
    out_buffer->height = img.height;
    return MESH_OK;
}

mesh_status_t mesh_render_png_file(
    const char* mesh_source,
    const char* output_png_path,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
) {
    if (out_error_message) *out_error_message = nullptr;
    if (!mesh_source || !output_png_path) return MESH_ERR_INVALID_ARG;

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene);
    if (!pr.ok) {
        if (out_error_message) *out_error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene(scene, img, override_width, override_height);
    if (!rr.ok) {
        if (out_error_message) *out_error_message = dup_cstr(rr.error);
        return MESH_ERR_EVAL;
    }

    mesh::Result wr = mesh::write_png_file(img, output_png_path);
    if (!wr.ok) {
        if (out_error_message) *out_error_message = dup_cstr(wr.error);
        return MESH_ERR_IO;
    }
    return MESH_OK;
}

mesh_status_t mesh_render_gif_file(
    const char* mesh_source,
    const char* output_gif_path,
    double duration_sec,
    int fps,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
) {
    if (out_error_message) *out_error_message = nullptr;
    if (!mesh_source || !output_gif_path) return MESH_ERR_INVALID_ARG;

    mesh::RenderOptions opts;
    opts.override_width = override_width;
    opts.override_height = override_height;
    opts.ssaa_samples = 1;
    opts.num_threads = 0;

    mesh::Result gr = mesh::render_animation_gif(mesh_source, output_gif_path, duration_sec, fps, opts);
    if (!gr.ok) {
        if (out_error_message) *out_error_message = dup_cstr(gr.error);
        return MESH_ERR_EVAL;
    }
    return MESH_OK;
}

mesh_status_t mesh_render_mp4_file(
    const char* mesh_source,
    const char* output_mp4_path,
    double duration_sec,
    int fps,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
) {
    if (out_error_message) *out_error_message = nullptr;
    if (!mesh_source || !output_mp4_path) return MESH_ERR_INVALID_ARG;

    mesh::RenderOptions opts;
    opts.override_width = override_width;
    opts.override_height = override_height;
    opts.ssaa_samples = 1;
    opts.num_threads = 0;

    mesh::Result mr = mesh::render_animation_mp4(mesh_source, output_mp4_path, duration_sec, fps, opts);
    if (!mr.ok) {
        if (out_error_message) *out_error_message = dup_cstr(mr.error);
        return MESH_ERR_EVAL;
    }
    return MESH_OK;
}

mesh_status_t mesh_pack_binary(
    const char* mesh_source,
    double time_sec,
    mesh_binary_result_t* out_binary
) {
    if (!mesh_source || !out_binary) return MESH_ERR_INVALID_ARG;
    std::memset(out_binary, 0, sizeof(*out_binary));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene, time_sec);
    if (!pr.ok) {
        out_binary->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    std::vector<uint8_t> bin;
    mesh::Result er = mesh::encode_meshb(scene, bin);
    if (!er.ok) {
        out_binary->error_message = dup_cstr(er.error);
        return MESH_ERR_EVAL;
    }

    out_binary->data = static_cast<uint8_t*>(std::malloc(bin.size()));
    if (!out_binary->data) return MESH_ERR_EVAL;
    std::memcpy(out_binary->data, bin.data(), bin.size());
    out_binary->length = bin.size();
    return MESH_OK;
}

mesh_status_t mesh_render_binary_rgba(
    const uint8_t* meshb_data,
    size_t meshb_length,
    uint32_t override_width,
    uint32_t override_height,
    mesh_image_buffer_t* out_buffer
) {
    if (!meshb_data || meshb_length == 0 || !out_buffer) return MESH_ERR_INVALID_ARG;
    std::memset(out_buffer, 0, sizeof(*out_buffer));

    std::vector<uint8_t> vec(meshb_data, meshb_data + meshb_length);
    mesh::Scene scene;
    mesh::Result dr = mesh::decode_meshb(vec, scene);
    if (!dr.ok) {
        out_buffer->error_message = dup_cstr(dr.error);
        return MESH_ERR_CORRUPT_BINARY;
    }

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene(scene, img, override_width, override_height);
    if (!rr.ok) {
        out_buffer->error_message = dup_cstr(rr.error);
        return MESH_ERR_EVAL;
    }

    size_t n = img.rgba.size();
    out_buffer->rgba_pixels = static_cast<uint8_t*>(std::malloc(n));
    if (!out_buffer->rgba_pixels) return MESH_ERR_EVAL;
    std::memcpy(out_buffer->rgba_pixels, img.rgba.data(), n);
    out_buffer->width = img.width;
    out_buffer->height = img.height;
    return MESH_OK;
}

mesh_status_t mesh_import_svg(
    const char* svg_xml_source,
    mesh_string_result_t* out_mesh_source
) {
    if (!svg_xml_source || !out_mesh_source) return MESH_ERR_INVALID_ARG;
    std::memset(out_mesh_source, 0, sizeof(*out_mesh_source));

    std::string msrc;
    mesh::Result ir = mesh::import_svg_to_mesh(svg_xml_source, msrc);
    if (!ir.ok) {
        out_mesh_source->error_message = dup_cstr(ir.error);
        return MESH_ERR_SYNTAX;
    }

    out_mesh_source->data = dup_cstr(msrc);
    out_mesh_source->length = msrc.size();
    return MESH_OK;
}

mesh_status_t mesh_compile_html(
    const char* mesh_source,
    mesh_string_result_t* out_html
) {
    if (!mesh_source || !out_html) return MESH_ERR_INVALID_ARG;
    std::memset(out_html, 0, sizeof(*out_html));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene);
    if (!pr.ok) {
        out_html->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    std::string html;
    mesh::Result cr = mesh::compile_to_html(scene, mesh_source, html);
    if (!cr.ok) {
        out_html->error_message = dup_cstr(cr.error);
        return MESH_ERR_EVAL;
    }

    out_html->data = dup_cstr(html);
    out_html->length = html.size();
    return MESH_OK;
}

mesh_status_t mesh_compile_svg(
    const char* mesh_source,
    mesh_string_result_t* out_svg
) {
    if (!mesh_source || !out_svg) return MESH_ERR_INVALID_ARG;
    std::memset(out_svg, 0, sizeof(*out_svg));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene);
    if (!pr.ok) {
        out_svg->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    std::string svg;
    mesh::Result cr = mesh::compile_to_svg(scene, svg);
    if (!cr.ok) {
        out_svg->error_message = dup_cstr(cr.error);
        return MESH_ERR_EVAL;
    }

    out_svg->data = dup_cstr(svg);
    out_svg->length = svg.size();
    return MESH_OK;
}

mesh_status_t mesh_compile_wgsl(
    const char* mesh_source,
    mesh_string_result_t* out_wgsl
) {
    if (!mesh_source || !out_wgsl) return MESH_ERR_INVALID_ARG;
    std::memset(out_wgsl, 0, sizeof(*out_wgsl));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene);
    if (!pr.ok) {
        out_wgsl->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    std::string wgsl;
    mesh::Result cr = mesh::compile_to_wgsl(scene, wgsl);
    if (!cr.ok) {
        out_wgsl->error_message = dup_cstr(cr.error);
        return MESH_ERR_EVAL;
    }

    out_wgsl->data = dup_cstr(wgsl);
    out_wgsl->length = wgsl.size();
    return MESH_OK;
}

mesh_status_t mesh_compile_glsl(
    const char* mesh_source,
    mesh_string_result_t* out_glsl
) {
    if (!mesh_source || !out_glsl) return MESH_ERR_INVALID_ARG;
    std::memset(out_glsl, 0, sizeof(*out_glsl));

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene);
    if (!pr.ok) {
        out_glsl->error_message = dup_cstr(pr.error);
        return MESH_ERR_SYNTAX;
    }

    std::string glsl;
    mesh::Result cr = mesh::compile_to_glsl(scene, glsl);
    if (!cr.ok) {
        out_glsl->error_message = dup_cstr(cr.error);
        return MESH_ERR_EVAL;
    }

    out_glsl->data = dup_cstr(glsl);
    out_glsl->length = glsl.size();
    return MESH_OK;
}

mesh_status_t mesh_format_source(
    const char* mesh_source,
    mesh_string_result_t* out_formatted
) {
    if (!mesh_source || !out_formatted) return MESH_ERR_INVALID_ARG;
    std::memset(out_formatted, 0, sizeof(*out_formatted));

    std::string fmt;
    mesh::Result fr = mesh::format_mesh_source(mesh_source, fmt);
    if (!fr.ok) {
        out_formatted->error_message = dup_cstr(fr.error);
        return MESH_ERR_SYNTAX;
    }

    out_formatted->data = dup_cstr(fmt);
    out_formatted->length = fmt.size();
    return MESH_OK;
}

void mesh_free_image_buffer(mesh_image_buffer_t* buffer) {
    if (!buffer) return;
    if (buffer->rgba_pixels) std::free(buffer->rgba_pixels);
    if (buffer->error_message) std::free(buffer->error_message);
    std::memset(buffer, 0, sizeof(*buffer));
}

void mesh_free_string_result(mesh_string_result_t* result) {
    if (!result) return;
    if (result->data) std::free(result->data);
    if (result->error_message) std::free(result->error_message);
    std::memset(result, 0, sizeof(*result));
}

void mesh_free_binary_result(mesh_binary_result_t* result) {
    if (!result) return;
    if (result->data) std::free(result->data);
    if (result->error_message) std::free(result->error_message);
    std::memset(result, 0, sizeof(*result));
}

void mesh_free_string(char* str) {
    if (str) std::free(str);
}

} // extern "C"

// ============================================================================
// 9. WebAssembly Zero-Copy Linear Memory & Shader Bindings
// ============================================================================

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define MESH_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(__wasm__)
#define MESH_WASM_EXPORT __attribute__((visibility("default")))
#else
#define MESH_WASM_EXPORT
#endif

namespace {

static std::vector<uint8_t> g_wasm_rgba_buffer;
static std::string g_wasm_last_string;
static std::string g_wasm_last_error;
static uint32_t g_wasm_last_width = 0;
static uint32_t g_wasm_last_height = 0;

} // namespace

extern "C" {

MESH_WASM_EXPORT const char* mesh_wasm_version(void) {
    return mesh::VERSION;
}

MESH_WASM_EXPORT uint8_t* mesh_wasm_alloc(size_t size) {
    return static_cast<uint8_t*>(std::malloc(size));
}

MESH_WASM_EXPORT void mesh_wasm_free(void* ptr) {
    if (ptr) std::free(ptr);
}

MESH_WASM_EXPORT const char* mesh_wasm_get_last_error(void) {
    return g_wasm_last_error.c_str();
}

MESH_WASM_EXPORT uint32_t mesh_wasm_get_width(void) {
    return g_wasm_last_width;
}

MESH_WASM_EXPORT uint32_t mesh_wasm_get_height(void) {
    return g_wasm_last_height;
}

// Evaluates and rasterizes `.mesh` source at timestamp `time_sec` into a persistent RGBA8 pixel buffer
// in Wasm linear memory, returning a direct pointer for zero-copy `new Uint8ClampedArray(memory.buffer, ptr, len)`.
MESH_WASM_EXPORT const uint8_t* mesh_wasm_render_frame(
    const char* mesh_source,
    double time_sec,
    uint32_t override_width,
    uint32_t override_height,
    int ssaa_samples
) {
    if (!mesh_source) {
        g_wasm_last_error = "Null mesh_source pointer";
        return nullptr;
    }

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene, time_sec);
    if (!pr.ok) {
        g_wasm_last_error = pr.error;
        return nullptr;
    }

    mesh::RenderOptions opts;
    opts.override_width = override_width;
    opts.override_height = override_height;
    opts.ssaa_samples = (ssaa_samples >= 4) ? 4 : 1;
    opts.num_threads = 1; // Single-threaded inside Wasm main worker

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene_ex(scene, img, opts);
    if (!rr.ok) {
        g_wasm_last_error = rr.error;
        return nullptr;
    }

    g_wasm_last_error.clear();
    g_wasm_last_width = img.width;
    g_wasm_last_height = img.height;
    g_wasm_rgba_buffer = std::move(img.rgba);
    return g_wasm_rgba_buffer.data();
}

MESH_WASM_EXPORT const char* mesh_wasm_compile_target(
    const char* mesh_source,
    const char* target_name,
    double time_sec
) {
    if (!mesh_source || !target_name) {
        g_wasm_last_error = "Null argument";
        return nullptr;
    }

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(mesh_source, scene, time_sec);
    if (!pr.ok) {
        g_wasm_last_error = pr.error;
        return nullptr;
    }

    std::string target(target_name);
    mesh::Result cr;
    if (target == "wgsl") {
        cr = mesh::compile_to_wgsl(scene, g_wasm_last_string);
    } else if (target == "glsl") {
        cr = mesh::compile_to_glsl(scene, g_wasm_last_string);
    } else if (target == "svg") {
        cr = mesh::compile_to_svg(scene, g_wasm_last_string);
    } else {
        cr = mesh::decompile_scene_to_mesh(scene, g_wasm_last_string);
    }

    if (!cr.ok) {
        g_wasm_last_error = cr.error;
        return nullptr;
    }
    g_wasm_last_error.clear();
    return g_wasm_last_string.c_str();
}

} // extern "C"
