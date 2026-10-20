// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "mesh.hpp"
#include "mesh_font.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static bool read_file_text(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return true;
}

static bool write_file_text(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return true;
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string dir_name_of(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

static const char* font_style_name(mesh::FontStyle style) {
    switch (style) {
        case mesh::FontStyle::Italic: return "italic";
        case mesh::FontStyle::Oblique: return "oblique";
        default: return "normal";
    }
}

static void print_usage() {
    std::cout << "meshc v" << mesh::VERSION << " — Next-Generation Declarative Vector Graphics Compiler & GPU/CPU Engine\n"
              << "License: Apache-2.0\n\n"
              << "Usage:\n"
              << "  meshc render   <input.mesh|input.meshb> -o <out.png|out.ppm> [--width W] [--height H] [--time T] [--aa 1|4] [--threads N]\n"
              << "  meshc animate  <input.mesh>  -o <out.gif|out.mp4|out.mov|out_sheet.png> [--duration D] [--fps F] [--width W] [--height H] [--aa 1|4] [--threads N]\n"
              << "  meshc compile  <input.mesh|input.meshb> -o <out.html|out.svg|out.wgsl|out.glsl> [--target html|svg|wgsl|glsl] [--time T]\n"
              << "  meshc pack     <input.mesh>  -o <output.meshb> [--time T]\n"
              << "  meshc unpack   <input.meshb> -o <output.mesh>\n"
              << "  meshc import   <input.svg>   -o <output.mesh>\n"
              << "  meshc bench    <input.mesh>  [--iterations N] [--aa 1|4] [--threads N]\n"
              << "  meshc inspect  <input.mesh|input.meshb> [--json] [--time T]\n"
              << "  meshc fmt      <input.mesh>  [-o <output.mesh>]\n"
              << "  meshc validate <input.mesh|input.meshb>\n"
              << "  meshc fonts    [--json]\n"
              << "  meshc --version\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "-v") {
        std::cout << "meshc " << mesh::VERSION << "\n";
        return 0;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "fonts" || cmd == "--fonts") {
        mesh::FontBackendStatus st = mesh::FontBackend::instance().status();
        bool json = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--json") json = true;
        }
        if (json) {
            std::cout << "{\"available\": " << (st.available ? "true" : "false")
                      << ", \"freetype\": \"" << json_escape(st.free_type_version)
                      << "\", \"harfbuzz\": \"" << json_escape(st.harfbuzz_version)
                      << "\", \"fontconfig\": \"" << json_escape(st.fontconfig_version)
                      << "\", \"message\": \"" << json_escape(st.message) << "\"}\n";
        } else {
            std::cout << "Real font backend: " << (st.available ? "available" : "unavailable") << "\n";
            if (!st.free_type_version.empty()) std::cout << "  FreeType:   " << st.free_type_version << "\n";
            if (!st.harfbuzz_version.empty()) std::cout << "  HarfBuzz:   " << st.harfbuzz_version << "\n";
            if (!st.fontconfig_version.empty()) std::cout << "  Fontconfig: " << st.fontconfig_version << "\n";
            if (!st.message.empty()) std::cout << "  " << st.message << "\n";
        }
        return st.available ? 0 : 1;
    }

    if (argc < 3) {
        std::cerr << "Error: Missing input file path.\n";
        print_usage();
        return 1;
    }

    std::string input_path = argv[2];
    std::string base_dir = dir_name_of(input_path);
    std::string output_path;
    std::string target;
    uint32_t override_w = 0;
    uint32_t override_h = 0;
    double eval_time = 0.0;
    double anim_duration = 2.0;
    int anim_fps = 15;
    int ssaa_samples = 1;
    int num_threads = 0;
    int bench_iterations = 5;
    bool json_output = false;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            target = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            override_w = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            override_h = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if ((arg == "--time" || arg == "-t") && i + 1 < argc) {
            eval_time = std::stod(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            anim_duration = std::stod(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            anim_fps = std::stoi(argv[++i]);
        } else if (arg == "--aa" && i + 1 < argc) {
            ssaa_samples = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            num_threads = std::stoi(argv[++i]);
        } else if (arg == "--iterations" && i + 1 < argc) {
            bench_iterations = std::stoi(argv[++i]);
        } else if (arg == "--json") {
            json_output = true;
        }
    }

    if (cmd == "import") {
        std::string svg_text;
        if (!read_file_text(input_path, svg_text)) {
            std::cerr << "Error: Cannot read input SVG file '" << input_path << "'\n";
            return 1;
        }
        std::string mesh_src;
        mesh::Result ir = mesh::import_svg_to_mesh(svg_text, mesh_src);
        if (!ir.ok) {
            std::cerr << "Import Error: " << ir.error << "\n";
            return 1;
        }
        if (output_path.empty()) {
            std::cout << mesh_src;
        } else if (!write_file_text(output_path, mesh_src)) {
            std::cerr << "Error: Cannot write output file '" << output_path << "'\n";
            return 1;
        } else {
            std::cout << "Imported SVG '" << input_path << "' -> '" << output_path << "'\n";
        }
        return 0;
    }

    if (cmd == "fmt") {
        std::string source;
        if (!read_file_text(input_path, source)) {
            std::cerr << "Error: Cannot read input file '" << input_path << "'\n";
            return 1;
        }
        std::string formatted;
        mesh::Result r = mesh::format_mesh_source(source, formatted, dir_name_of(input_path));
        if (!r.ok) {
            std::cerr << "Format Error: " << r.error << "\n";
            return 1;
        }
        if (output_path.empty()) {
            std::cout << formatted;
        } else if (!write_file_text(output_path, formatted)) {
            std::cerr << "Error: Cannot write output file '" << output_path << "'\n";
            return 1;
        }
        return 0;
    }

    if (cmd == "bench") {
        std::string source_text;
        if (!read_file_text(input_path, source_text)) {
            std::cerr << "Error: Cannot read input file '" << input_path << "'\n";
            return 1;
        }
        mesh::RenderOptions ropts;
        ropts.override_width = override_w;
        ropts.override_height = override_h;
        ropts.ssaa_samples = ssaa_samples;
        ropts.num_threads = num_threads;

        mesh::BenchmarkStats stats{};
        mesh::Result br = mesh::benchmark_scene(source_text, bench_iterations, ropts, stats, base_dir);
        if (!br.ok) {
            std::cerr << "Benchmark Error: " << br.error << "\n";
            return 1;
        }
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Mesh Benchmark (" << bench_iterations << " iterations, AA=" << ssaa_samples << "x):\n"
                  << "  Parse + Layout + BBox Solve: " << stats.parse_layout_ms << " ms\n"
                  << "  64x64 Tile-Binned Raster:    " << stats.render_ms << " ms\n"
                  << "  Total Frame Time:            " << stats.total_ms << " ms (" << (1000.0 / std::max(0.01, stats.total_ms)) << " FPS)\n"
                  << "  Throughput:                  " << stats.megapixels_per_sec << " MPix/s\n"
                  << "  Leaf Nodes / Active Tiles:   " << stats.leaf_nodes << " leaves / " << stats.active_tile_bins << " tiles\n";
        return 0;
    }

    if (cmd == "animate") {
        std::string source_text;
        if (!read_file_text(input_path, source_text)) {
            std::cerr << "Error: Cannot read input file '" << input_path << "'\n";
            return 1;
        }
        if (output_path.empty()) output_path = "animation.gif";
        mesh::RenderOptions ropts;
        ropts.override_width = override_w;
        ropts.override_height = override_h;
        ropts.ssaa_samples = ssaa_samples;
        ropts.num_threads = num_threads;

        auto t0 = std::chrono::steady_clock::now();
        mesh::Result ar;
        if (ends_with(output_path, ".png")) {
            ar = mesh::render_animation_spritesheet(source_text, output_path, anim_duration, anim_fps, ropts, base_dir);
        } else if (ends_with(output_path, ".mp4") || ends_with(output_path, ".mov")) {
            ar = mesh::render_animation_mp4(source_text, output_path, anim_duration, anim_fps, ropts, base_dir);
        } else {
            ar = mesh::render_animation_gif(source_text, output_path, anim_duration, anim_fps, ropts, base_dir);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!ar.ok) {
            std::cerr << "Animation Error: " << ar.error << "\n";
            return 1;
        }
        int total_frames = std::max(1, static_cast<int>(std::round(anim_duration * anim_fps)));
        std::cout << "Animated '" << input_path << "' -> '" << output_path << "' ("
                  << total_frames << " frames @ " << anim_fps << " FPS, "
                  << std::fixed << std::setprecision(1) << ms << " ms)\n";
        return 0;
    }

    bool is_binary_input = ends_with(input_path, ".meshb");
    mesh::Scene scene;
    std::string source_text;

    if (is_binary_input) {
        mesh::Result br = mesh::read_meshb_file(input_path, scene);
        if (!br.ok) {
            std::cerr << "Binary Decode Error in '" << input_path << "': " << br.error << "\n";
            return 1;
        }
        mesh::decompile_scene_to_mesh(scene, source_text);
    } else {
        if (!read_file_text(input_path, source_text)) {
            std::cerr << "Error: Cannot read input file '" << input_path << "'\n";
            return 1;
        }
        mesh::Result pr = mesh::parse_and_evaluate(source_text, scene, eval_time, base_dir);
        if (!pr.ok) {
            std::cerr << "Syntax/Evaluation Error in '" << input_path << "':\n  " << pr.error << "\n";
            return 1;
        }
    }

    if (cmd == "pack") {
        if (output_path.empty()) output_path = "output.meshb";
        mesh::Result wr = mesh::write_meshb_file(scene, output_path);
        if (!wr.ok) {
            std::cerr << "Pack Error: " << wr.error << "\n";
            return 1;
        }
        std::cout << "Packed '" << input_path << "' -> '" << output_path << "' (binary .meshb v1.0 + CRC32)\n";
        return 0;
    }

    if (cmd == "unpack") {
        std::string decompiled;
        mesh::Result dr = mesh::decompile_scene_to_mesh(scene, decompiled);
        if (!dr.ok) {
            std::cerr << "Unpack Error: " << dr.error << "\n";
            return 1;
        }
        if (output_path.empty()) {
            std::cout << decompiled;
        } else if (!write_file_text(output_path, decompiled)) {
            std::cerr << "Error: Cannot write output file '" << output_path << "'\n";
            return 1;
        } else {
            std::cout << "Unpacked '" << input_path << "' -> '" << output_path << "'\n";
        }
        return 0;
    }

    if (cmd == "validate") {
        std::cout << "OK: '" << input_path << "' (mesh \"" << scene.name << "\", "
                  << scene.width << "x" << scene.height << ", "
                  << scene.nodes.size() << " top-level nodes, "
                  << scene.defs.size() << " defs, "
                  << scene.fonts.size() << " fonts, default font \"" << scene.default_font_family << "\", "
                  << scene.node_bboxes.size() << " relational @id anchors)\n";
        return 0;
    }

    if (cmd == "inspect") {
        if (json_output) {
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "{\n  \"name\": \"" << scene.name << "\",\n"
                      << "  \"width\": " << scene.width << ",\n"
                      << "  \"height\": " << scene.height << ",\n"
                      << "  \"eval_time\": " << scene.eval_time << ",\n"
                      << "  \"defs_count\": " << scene.defs.size() << ",\n"
                      << "  \"default_font\": \"" << json_escape(scene.default_font_family) << "\",\n"
                      << "  \"fonts\": {\n";
            bool first_f = true;
            for (const auto& [id, spec] : scene.fonts) {
                if (!first_f) std::cout << ",\n";
                first_f = false;
                std::cout << "    \"" << json_escape(id) << "\": {\"family\": \"" << json_escape(spec.family)
                          << "\", \"src\": \"" << json_escape(spec.src) << "\", \"fallback\": \""
                          << json_escape(spec.fallback) << "\", \"weight\": " << spec.weight
                          << ", \"style\": \"" << font_style_name(spec.style) << "\", \"stretch\": "
                          << spec.stretch << ", \"index\": " << spec.face_index << "}";
            }
            std::cout << "\n  },\n  \"variables\": {\n";
            bool first_v = true;
            for (const auto& [k, v] : scene.variables) {
                if (!first_v) std::cout << ",\n";
                first_v = false;
                std::cout << "    \"" << k << "\": " << v;
            }
            std::cout << "\n  },\n  \"anchors\": {\n";
            bool first_a = true;
            for (const auto& [id, bb] : scene.node_bboxes) {
                if (!bb.valid) continue;
                if (!first_a) std::cout << ",\n";
                first_a = false;
                std::cout << "    \"@" << id << "\": {\"x\": " << bb.min_x << ", \"y\": " << bb.min_y
                          << ", \"width\": " << bb.width() << ", \"height\": " << bb.height()
                          << ", \"center_x\": " << bb.center_x() << ", \"center_y\": " << bb.center_y() << "}";
            }
            std::cout << "\n  }\n}\n";
        } else {
            std::cout << "Mesh Document: \"" << scene.name << "\" (" << scene.width << "x" << scene.height
                      << ", t=" << scene.eval_time << "s)\n";
            std::cout << "Paint Servers: " << scene.paint_servers.size() << " | Reusable Defs: " << scene.defs.size() << "\n";
            std::cout << "Fonts: " << scene.fonts.size() << " | Default: \"" << scene.default_font_family
                      << "\" (fallback \"" << scene.default_font_fallback << "\")\n";
            for (const auto& [id, spec] : scene.fonts) {
                std::cout << "  @" << std::left << std::setw(14) << id << " family=\"" << spec.family
                          << "\" weight=" << spec.weight << " style=" << font_style_name(spec.style);
                if (!spec.src.empty()) std::cout << " src=\"" << spec.src << "\"";
                if (!spec.fallback.empty()) std::cout << " fallback=\"" << spec.fallback << "\"";
                std::cout << "\n";
            }
            std::cout << "Resolved Relational @id Anchors (" << scene.node_bboxes.size() << "):\n";
            for (const auto& [id, bb] : scene.node_bboxes) {
                if (!bb.valid) continue;
                std::cout << "  @" << std::left << std::setw(20) << id
                          << " bbox=(" << bb.min_x << ", " << bb.min_y << ", " << bb.width() << "x" << bb.height() << ")"
                          << " center=(" << bb.center_x() << ", " << bb.center_y() << ")\n";
            }
        }
        return 0;
    }

    if (cmd == "render") {
        if (output_path.empty()) output_path = "output.png";
        mesh::RenderOptions opts;
        opts.override_width = override_w;
        opts.override_height = override_h;
        opts.ssaa_samples = ssaa_samples;
        opts.num_threads = num_threads;

        auto t0 = std::chrono::steady_clock::now();
        mesh::RasterImage img;
        mesh::Result rr = mesh::render_scene_ex(scene, img, opts);
        if (!rr.ok) {
            std::cerr << "Render Error: " << rr.error << "\n";
            return 1;
        }
        bool is_ppm = ends_with(output_path, ".ppm");
        mesh::Result wr = is_ppm ? mesh::write_ppm_file(img, output_path)
                                 : mesh::write_png_file(img, output_path);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!wr.ok) {
            std::cerr << "IO Error: " << wr.error << "\n";
            return 1;
        }
        std::cout << "Rendered '" << input_path << "' -> '" << output_path << "' ("
                  << img.width << "x" << img.height << " RGBA, AA=" << ssaa_samples << "x, "
                  << std::fixed << std::setprecision(1) << ms << " ms)\n";
        return 0;
    }

    if (cmd == "compile") {
        if (target.empty()) {
            if (ends_with(output_path, ".svg")) target = "svg";
            else if (ends_with(output_path, ".wgsl")) target = "wgsl";
            else if (ends_with(output_path, ".glsl") || ends_with(output_path, ".frag")) target = "glsl";
            else target = "html";
        }
        if (output_path.empty()) {
            if (target == "svg") output_path = "output.svg";
            else if (target == "wgsl") output_path = "output.wgsl";
            else if (target == "glsl") output_path = "output.glsl";
            else output_path = "output.html";
        }

        std::string compiled;
        mesh::Result cr;
        if (target == "svg") {
            cr = mesh::compile_to_svg(scene, compiled);
        } else if (target == "wgsl") {
            cr = mesh::compile_to_wgsl(scene, compiled);
        } else if (target == "glsl") {
            cr = mesh::compile_to_glsl(scene, compiled);
        } else {
            cr = mesh::compile_to_html(scene, source_text, compiled);
        }
        if (!cr.ok) {
            std::cerr << "Compile Error: " << cr.error << "\n";
            return 1;
        }
        if (!write_file_text(output_path, compiled)) {
            std::cerr << "IO Error: Cannot write to '" << output_path << "'\n";
            return 1;
        }
        std::cout << "Compiled '" << input_path << "' -> '" << output_path << "' (target: " << target << ")\n";
        return 0;
    }

    std::cerr << "Error: Unknown command '" << cmd << "'\n";
    print_usage();
    return 1;
}
