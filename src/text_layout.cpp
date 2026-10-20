// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "mesh.hpp"
#include "mesh_font.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace mesh {

namespace {

static std::string trim_copy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static void split_family_list(const std::string& value, std::string& first, std::string& rest) {
    first.clear();
    rest.clear();
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string part = trim_copy(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!part.empty()) {
            if (first.empty()) first = part;
            else {
                if (!rest.empty()) rest += ", ";
                rest += part;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

static void apply_translation(std::vector<FlattenedSubpath>& subpaths, double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) return;
    for (auto& sp : subpaths) {
        for (auto& pt : sp.points) {
            pt.x += dx;
            pt.y += dy;
        }
    }
}

static Result resolve_request(const ShapeNode& node, const Scene& scene, FontRequest& req) {
    const FontSpec* spec = nullptr;
    if (!node.font_id.empty()) {
        auto it = scene.fonts.find(node.font_id);
        if (it == scene.fonts.end()) {
            return {false, "Unknown font '" + node.font_id + "' (declare it with font \"" +
                               node.font_id + "\" { ... } before use)", 0, 0};
        }
        spec = &it->second;
    }

    bool explicit_font = (spec != nullptr) || !node.font_family.empty() || !node.font_src.empty();

    if (spec) {
        req.family = spec->family;
        req.src = spec->src;
        req.fallback = spec->fallback;
        req.weight = spec->weight;
        req.slant = static_cast<int>(spec->style);
        req.stretch = spec->stretch;
        req.face_index = spec->face_index;
    } else {
        req.family = node.font_family;
        req.src = node.font_src;
        req.fallback = node.font_fallback;
        req.weight = 400.0;
        req.slant = static_cast<int>(FontStyle::Normal);
        req.stretch = 100.0;
        req.face_index = 0;
    }

    if (!node.font_family.empty()) req.family = node.font_family;
    if (!node.font_src.empty()) req.src = node.font_src;
    if (!node.font_fallback.empty()) req.fallback = node.font_fallback;
    if (node.font_weight_explicit || !spec) req.weight = node.font_weight;
    if (node.font_style_explicit || !spec) req.slant = static_cast<int>(node.font_style);
    if (node.font_stretch_explicit || !spec) req.stretch = node.font_stretch;

    if (req.family.empty() && req.src.empty()) {
        req.family = scene.default_font_family.empty() ? "Noto Sans" : scene.default_font_family;
        explicit_font = false;
    }
    if (!req.family.empty()) {
        std::string first;
        std::string rest;
        split_family_list(req.family, first, rest);
        if (!first.empty()) req.family = first;
        if (!rest.empty()) {
            req.fallback = req.fallback.empty() ? rest : (rest + ", " + req.fallback);
        }
    }
    if (req.fallback.empty()) {
        req.fallback = scene.default_font_fallback.empty() ? "DejaVu Sans" : scene.default_font_fallback;
    }

    // The built-in stroke font is still a valid deterministic fallback for
    // documents that did not ask for a specific face.
    if (!FontBackend::instance().status().available && explicit_font) {
        return {false,
                "Real font support is unavailable in this build; cannot load requested font '" +
                    (spec ? spec->id : req.family) + "'",
                0, 0};
    }
    return {true, "", 0, 0};
}

} // namespace

Result layout_text_node(ShapeNode& node, const Scene& scene, const std::string& base_dir) {
    if (node.kind != ShapeKind::Text) return {true, "", 0, 0};

    node.text_subpaths.clear();
    node.text_width = 0.0;
    node.text_ascent = 0.0;
    node.text_descent = 0.0;
    node.text_line_height = 0.0;
    node.text_line_count = 0;

    if (node.text_content.empty()) return {true, "", 0, 0};

    FontRequest freq;
    Result rr = resolve_request(node, scene, freq);
    if (!rr.ok) return rr;

    if (!FontBackend::instance().status().available) {
        // No real backend: leave text_subpaths empty so the rasterizer uses its
        // built-in vector font. Explicit requests above already failed loudly.
        if (node.style.fill.kind == PaintKind::None && node.style.stroke.kind == PaintKind::None) {
            node.style.fill.kind = PaintKind::Solid;
            node.style.fill.solid_color = Color::from_srgb(0, 0, 0, 1);
        }
        return {true, "", 0, 0};
    }

    TextRequest treq;
    treq.text = node.text_content;
    treq.size = std::max(0.1, node.font_size);
    treq.letter_spacing = node.letter_spacing;
    treq.word_spacing = node.word_spacing;
    treq.line_height = std::max(0.1, node.line_height);
    treq.max_width = std::max(0.0, node.max_width);
    treq.align = static_cast<int>(node.text_align);
    treq.wrap = node.wrap && treq.max_width > 0.0;
    treq.features = node.font_features;
    treq.language = node.font_language;

    TextLayout layout;
    Result lr = FontBackend::instance().layout_text(freq,
                                                    treq,
                                                    base_dir.empty() ? scene.base_dir : base_dir,
                                                    layout);
    if (!lr.ok) {
        return {false, "Font layout failed for text \"" + node.text_content + "\": " + lr.error, 0, 0};
    }

    if (layout.subpaths.empty()) {
        // Whitespace, or a face with no usable outlines. Keep the built-in
        // fallback path empty so nothing is drawn twice.
        node.text_width = layout.advance_width;
        node.text_ascent = layout.ascent;
        node.text_descent = layout.descent;
        node.text_line_height = layout.line_height;
        node.text_line_count = layout.line_count;
        node.resolved_font_family = layout.resolved_family;
        node.resolved_font_path = layout.resolved_path;
        if (node.style.fill.kind == PaintKind::None && node.style.stroke.kind == PaintKind::None) {
            node.style.fill.kind = PaintKind::Solid;
            node.style.fill.solid_color = Color::from_srgb(0, 0, 0, 1);
        }
        return {true, "", 0, 0};
    }

    // The layout places the first baseline at y=0 and anchors horizontally
    // according to alignment. By default `at:` addresses the visual centre of
    // the text, matching Mesh's existing UI-oriented semantics. `baseline: true`
    // opts into classic SVG baseline positioning.
    double dy = node.baseline_anchor ? 0.0 : -layout.ink_bbox.center_y();
    apply_translation(layout.subpaths, node.text_pos.x, node.text_pos.y + dy);

    node.text_subpaths = std::move(layout.subpaths);
    node.text_width = layout.advance_width;
    node.text_ascent = layout.ascent;
    node.text_descent = layout.descent;
    node.text_line_height = layout.line_height;
    node.text_line_count = layout.line_count;
    node.resolved_font_family = layout.resolved_family;
    node.resolved_font_path = layout.resolved_path;

    if (node.style.fill.kind == PaintKind::None && node.style.stroke.kind == PaintKind::None) {
        node.style.fill.kind = PaintKind::Solid;
        node.style.fill.solid_color = Color::from_srgb(0, 0, 0, 1);
    }
    return {true, "", 0, 0};
}

static Result layout_recursive(ShapeNode& node, const Scene& scene, const std::string& base_dir) {
    if (node.kind == ShapeKind::Text && !node.text_content.empty()) {
        Result r = layout_text_node(node, scene, base_dir);
        if (!r.ok) return r;
    }
    for (auto& child : node.children) {
        Result r = layout_recursive(child, scene, base_dir);
        if (!r.ok) return r;
    }
    return {true, "", 0, 0};
}

Result layout_scene_text(Scene& scene) {
    for (auto& node : scene.nodes) {
        Result r = layout_recursive(node, scene, scene.base_dir);
        if (!r.ok) return r;
    }
    for (auto& entry : scene.defs) {
        Result r = layout_recursive(entry.second, scene, scene.base_dir);
        if (!r.ok) return r;
    }
    recompute_scene_Caches(scene);
    return {true, "", 0, 0};
}

} // namespace mesh
