// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#ifndef MESH_FONT_HPP
#define MESH_FONT_HPP

#include "mesh.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mesh {

// Font selection request. Either `src` (an explicit TTF/OTF/TTC file) or
// `family` (resolved through Fontconfig) is used. If both are supplied, `src`
// wins. `fallback` may contain one or more comma-separated family names used
// for per-character fallback before Fontconfig's normal sorted match list.
struct FontRequest {
    std::string family;        // requested family, e.g. "Segoe UI"
    std::string src;           // optional explicit TTF/OTF/TTC path
    std::string fallback;      // optional fallback family/families
    double weight = 400.0;     // 100..900
    int slant = 0;             // 0 normal, 1 italic, 2 oblique
    double stretch = 100.0;    // percent
    int face_index = 0;        // index inside a TTC/OTC collection
};

struct TextLineMetrics {
    double width = 0.0;        // advance width of this line in pixels
    double baseline = 0.0;     // y relative to the first baseline (y down)
    std::size_t start = 0;     // byte range in the UTF-8 input
    std::size_t end = 0;
};

struct TextLayout {
    std::vector<FlattenedSubpath> subpaths;
    BBox ink_bbox;             // tight outline bounds; invalid for empty text
    double advance_width = 0.0; // widest line's advance
    double ascent = 0.0;       // positive, scaled font ascent
    double descent = 0.0;      // positive, scaled font descent
    double line_height = 0.0;  // baseline-to-baseline distance in pixels
    int line_count = 0;        // 0 for empty input; "\n" alone yields 2 empty lines
    std::vector<TextLineMetrics> lines;
    std::string resolved_family;
    std::string resolved_path;
};

struct TextRequest {
    std::string text;
    double size = 18.0;
    double letter_spacing = 0.0; // pixels added after every shaped glyph
    double word_spacing = 0.0;   // extra pixels after ASCII spaces
    double line_height = 1.25;   // multiplier of font size
    double max_width = 0.0;      // 0 = no wrapping
    int align = 0;               // 0 left, 1 center, 2 right
    bool wrap = false;
    std::string features;        // e.g. "kern,liga,ss01"
    std::string language;        // BCP47 tag, optional
};

struct FontBackendStatus {
    bool available = false;
    std::string free_type_version;
    std::string harfbuzz_version;
    std::string fontconfig_version;
    std::string message;
};

class FontBackend {
public:
    static FontBackend& instance();

    FontBackendStatus status() const;
    bool available() const { return status().available; }

    // Shapes and outlines `text` with `font`.
    //   * The first baseline is y = 0.
    //   * x = 0 is the alignment anchor: left edge, centre of the advance,
    //     or right edge according to TextRequest::align.
    //   * `base_dir` is tried first for relative FontRequest::src paths, then
    //     the process working directory.
    Result layout_text(const FontRequest& font, const TextRequest& text,
                       const std::string& base_dir, TextLayout& out);

    // Resolves a request without shaping. Useful for diagnostics.
    Result resolve_font(const FontRequest& font, const std::string& base_dir,
                        std::string& out_path, int& out_face_index,
                        std::string& out_family, std::string& out_style);

private:
    FontBackend();
    ~FontBackend();
    FontBackend(const FontBackend&) = delete;
    FontBackend& operator=(const FontBackend&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mesh

#endif // MESH_FONT_HPP
