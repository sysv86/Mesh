// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "mesh.hpp"
#include "mesh_c_api.h"
#include "mesh_font.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                        \
    do {                                                                              \
        if (cond) {                                                                   \
            g_passed++;                                                               \
        } else {                                                                      \
            g_failed++;                                                               \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " - " << (msg) << "\n"; \
        }                                                                             \
    } while (0)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT_TRUE(std::abs((a) - (b)) <= (eps), msg)

static void test_oklch_color_science() {
    mesh::Color c = mesh::Color::from_oklch(0.75, 0.16, 210.0, 1.0);
    ASSERT_TRUE(c.r >= 0.0 && c.r <= 1.0, "OKLCH red channel in [0,1]");
    ASSERT_TRUE(c.g >= 0.0 && c.g <= 1.0, "OKLCH green channel in [0,1]");
    ASSERT_TRUE(c.b >= 0.0 && c.b <= 1.0, "OKLCH blue channel in [0,1]");
    ASSERT_NEAR(c.ok_L, 0.75, 1e-4, "OKLCH lightness preserved");

    mesh::Color c0 = mesh::Color::from_hex("#FF0055");
    mesh::Color c1 = mesh::Color::from_hex("#00FFCC");
    mesh::Color mid = mesh::Color::lerp_oklab(c0, c1, 0.5);
    ASSERT_NEAR(mid.ok_L, 0.5 * (c0.ok_L + c1.ok_L), 1e-4, "OKLab midpoint L is linear");
}

static void test_relational_layout_and_anchors() {
    const char* src = R"(
        mesh "layout_test" {
          canvas { width: 600; height: 400 }
          let pad = 24
          rect "box_a" {
            x: pad; y: 50; width: 120; height: 80
            fill: #336699
          }
          rect "box_b" {
            x: @box_a.right + pad
            y: @box_a.center_y - 20
            width: @box_a.width * 1.5
            height: 40
            fill: #66AAFF
          }
          circle "dot" {
            center: @box_b.right_center + (30, 0)
            radius: 15
            fill: oklch(0.8, 0.15, 140)
          }
        }
    )";

    mesh::Scene scene;
    mesh::Result r = mesh::parse_and_evaluate(src, scene);
    ASSERT_TRUE(r.ok, "Parse relational layout scene");
    ASSERT_TRUE(scene.node_bboxes.count("box_a") == 1, "box_a registered");
    ASSERT_TRUE(scene.node_bboxes.count("box_b") == 1, "box_b registered");
    ASSERT_TRUE(scene.node_bboxes.count("dot") == 1, "dot registered");

    const auto& ba = scene.node_bboxes["box_a"];
    const auto& bb = scene.node_bboxes["box_b"];
    const auto& bd = scene.node_bboxes["dot"];

    ASSERT_NEAR(ba.min_x, 24.0, 1e-4, "box_a min_x == 24");
    ASSERT_NEAR(ba.max_x, 144.0, 1e-4, "box_a max_x == 144");
    ASSERT_NEAR(bb.min_x, 168.0, 1e-4, "box_b min_x == @box_a.right + 24 == 168");
    ASSERT_NEAR(bb.width(), 180.0, 1e-4, "box_b width == 180");
    ASSERT_NEAR(bb.center_y(), ba.center_y(), 1e-4, "box_b vertically centered with box_a");
    ASSERT_NEAR(bd.center_x(), bb.max_x + 30.0, 1e-4, "dot center_x == @box_b.right + 30");
}

static void test_sdf_boolean_csg_and_splines() {
    const char* src = R"(
        mesh "csg_test" {
          canvas { width: 300; height: 300; background: #10141d }
          boolean "ring" {
            op: subtract
            fill: oklch(0.75, 0.18, 200)
            circle { center: (150, 150); radius: 80 }
            circle { center: (150, 150); radius: 40 }
          }
          spline "curve" {
            points: [(30, 260), (150, 220), (270, 260)]
            tension: 0.5
            stroke: #ffffff
            stroke_width: 4
          }
        }
    )";

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(src, scene);
    ASSERT_TRUE(pr.ok, "Parse CSG & spline scene");

    double d_center = mesh::evaluate_shape_sdf(scene.nodes[0], {150.0, 150.0});
    double d_ring = mesh::evaluate_shape_sdf(scene.nodes[0], {210.0, 150.0});
    ASSERT_TRUE(d_center > 0.0, "Subtracted hole center has positive exterior SDF");
    ASSERT_TRUE(d_ring < 0.0, "Ring body has negative interior SDF");

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene(scene, img);
    ASSERT_TRUE(rr.ok, "Rasterize CSG & spline scene");
    ASSERT_TRUE(img.width == 300 && img.height == 300, "Output dimensions 300x300");

    size_t idx_hole = (150 * 300 + 150) * 4;
    size_t idx_ring = (150 * 300 + 210) * 4;
    ASSERT_TRUE(img.rgba[idx_hole + 0] < 30, "Hole pixel matches dark background");
    ASSERT_TRUE(img.rgba[idx_ring + 2] > 150, "Ring pixel has bright cyan fill");
}

static void test_wave2_def_use_repeat_clip_and_time() {
    const char* src = R"(
        mesh "wave2_features" {
          canvas { width: 400; height: 400; background: #0d1117 }

          def "blade" {
            rect {
              x: -8; y: -60; width: 16; height: 40; radius: 4
              fill: oklch(0.80, 0.16, 190)
            }
          }

          use "first_blade" {
            target: @blade
            at: (200, 200)
          }

          repeat "rotor" {
            count: 6
            pivot: (200, 200)
            step_rotate: 60deg
            use {
              target: @blade
              at: (200, 200)
            }
          }

          circle "mask_circle" {
            center: (200, 200)
            radius: oscillate(t, 1.0, 50, 90)
            fill: #1f2937
          }

          rect "clipped_bar" {
            clip: @mask_circle
            blend: add
            x: 50; y: 185; width: 300; height: 30
            fill: oklch(0.85, 0.20, 145)
          }
        }
    )";

    mesh::Scene scene_t0;
    mesh::Result r0 = mesh::parse_and_evaluate(src, scene_t0, 0.0);
    ASSERT_TRUE(r0.ok, "Parse Wave 2 scene at t=0.0");
    ASSERT_TRUE(scene_t0.defs.count("blade") == 1, "def 'blade' registered");
    ASSERT_TRUE(scene_t0.node_bboxes.count("first_blade") == 1, "use 'first_blade' anchor registered");
    ASSERT_TRUE(scene_t0.node_bboxes.count("rotor") == 1, "repeat 'rotor' anchor registered");

    mesh::Scene scene_t25;
    mesh::Result r1 = mesh::parse_and_evaluate(src, scene_t25, 0.25);
    ASSERT_TRUE(r1.ok, "Parse Wave 2 scene at t=0.25");

    double r_at_0 = scene_t0.node_bboxes["mask_circle"].width() * 0.5;
    double r_at_025 = scene_t25.node_bboxes["mask_circle"].width() * 0.5;
    ASSERT_NEAR(r_at_0, 70.0, 1e-3, "oscillate(0, 1Hz, 50, 90) == midpoint 70");
    ASSERT_NEAR(r_at_025, 90.0, 1e-3, "oscillate(0.25, 1Hz, 50, 90) == peak 90");

    mesh::RenderOptions opts;
    opts.ssaa_samples = 4;
    opts.num_threads = 2;
    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene_ex(scene_t25, img, opts);
    ASSERT_TRUE(rr.ok, "Multithreaded 4x RGSS render succeeded");

    size_t idx_outside = (200 * 400 + 60) * 4;
    size_t idx_inside = (200 * 400 + 200) * 4;
    ASSERT_TRUE(img.rgba[idx_outside + 1] < 30, "Pixel outside @mask_circle is clipped");
    ASSERT_TRUE(img.rgba[idx_inside + 1] > 180, "Pixel inside @mask_circle has additive green bar");
}

static void test_wave2_binary_meshb_and_svg_import() {
    const char* src = R"(
        mesh "binary_roundtrip" {
          canvas { width: 256; height: 256; background: #111827 }
          circle "orb" {
            center: (128, 128); radius: 64
            fill: oklch(0.78, 0.18, 200)
            stroke: #ffffff; stroke_width: 4; stroke_align: inner
          }
        }
    )";

    mesh_binary_result_t bin{};
    mesh_status_t st = mesh_pack_binary(src, 0.0, &bin);
    ASSERT_TRUE(st == MESH_OK, "mesh_pack_binary succeeded");
    ASSERT_TRUE(bin.length > 32, ".meshb payload non-empty");
    ASSERT_TRUE(std::memcmp(bin.data, "MSHB", 4) == 0, ".meshb starts with MSHB magic");

    mesh_image_buffer_t img_buf{};
    st = mesh_render_binary_rgba(bin.data, bin.length, 0, 0, &img_buf);
    ASSERT_TRUE(st == MESH_OK, "mesh_render_binary_rgba succeeded");
    ASSERT_TRUE(img_buf.width == 256 && img_buf.height == 256, "Binary render dimensions 256x256");

    uint8_t orig_byte = bin.data[bin.length - 1];
    bin.data[bin.length - 1] ^= 0xFF;
    mesh_image_buffer_t bad_buf{};
    st = mesh_render_binary_rgba(bin.data, bin.length, 0, 0, &bad_buf);
    ASSERT_TRUE(st == MESH_ERR_CORRUPT_BINARY, "Corrupted .meshb caught by CRC32 checksum");
    bin.data[bin.length - 1] = orig_byte;

    mesh_free_image_buffer(&bad_buf);
    mesh_free_image_buffer(&img_buf);
    mesh_free_binary_result(&bin);

    const char* legacy_svg = R"(
        <svg viewBox="0 0 200 150" xmlns="http://www.w3.org/2000/svg">
          <rect id="card" x="10" y="15" width="180" height="120" rx="12" fill="#1e293b" stroke="#38bdf8" stroke-width="2"/>
          <circle id="badge" cx="100" cy="75" r="30" fill="#f43f5e"/>
          <path id="check" d="M 85 75 L 96 86 L 118 64" fill="none" stroke="#ffffff" stroke-width="3"/>
        </svg>
    )";

    mesh_string_result_t imported{};
    st = mesh_import_svg(legacy_svg, &imported);
    ASSERT_TRUE(st == MESH_OK, "mesh_import_svg succeeded");
    std::string msrc(imported.data, imported.length);
    ASSERT_TRUE(msrc.find("rect \"card\"") != std::string::npos, "Imported SVG contains rect 'card'");
    ASSERT_TRUE(msrc.find("circle \"badge\"") != std::string::npos, "Imported SVG contains circle 'badge'");

    mesh::Scene imported_scene;
    mesh::Result pr = mesh::parse_and_evaluate(msrc, imported_scene);
    ASSERT_TRUE(pr.ok, "Imported .mesh source parses and evaluates cleanly");
    ASSERT_NEAR(imported_scene.node_bboxes["badge"].center_x(), 100.0, 1e-3, "Imported badge center_x == 100");
    mesh_free_string_result(&imported);
}

static void test_wave3_auto_layout_noise_dashes_arrows_and_gpu_shaders() {
    const char* src = R"(
        mesh "wave3_test" {
          canvas { width: 500; height: 360; background: #0f172a }

          noise_gradient "aurora_tex" {
            scale: 4.0; octaves: 4; persistence: 0.5; seed: 7; grain: 0.02
            stop(0.0, oklch(0.18, 0.06, 250))
            stop(1.0, oklch(0.75, 0.16, 175))
          }

          stack "top_bar" {
            x: 20; y: 20; direction: horizontal; gap: 30; padding: 10
            fill: @aurora_tex; radius: 10
            rect "item_1" { width: 100; height: 50; fill: #1e293b }
            rect "item_2" { width: 120; height: 50; fill: #334155 }
          }

          grid "card_grid" {
            x: 20; y: @top_bar.bottom + 20; cols: 2; gap: (20, 16); padding: 12
            rect "g1" { width: 140; height: 60; fill: #1e293b }
            rect "g2" { width: 140; height: 60; fill: #1e293b }
            rect "g3" { width: 140; height: 60; fill: #1e293b }
            rect "g4" { width: 140; height: 60; fill: #1e293b }
          }

          line "dashed_arrow" {
            from: @item_1.right_center
            to: @item_2.left_center
            stroke: #38bdf8
            stroke_width: 3
            dash: [6, 4]
            dash_offset: 2
            arrow: end
            arrow_size: 10
          }
        }
    )";

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(src, scene);
    ASSERT_TRUE(pr.ok, "Wave 3 scene parses cleanly");

    ASSERT_NEAR(scene.node_bboxes["item_1"].min_x, 30.0, 1e-3, "stack item_1 x = 20 + padding(10) = 30");
    ASSERT_NEAR(scene.node_bboxes["item_2"].min_x, 160.0, 1e-3, "stack item_2 x = 30 + 100 + gap(30) = 160");
    ASSERT_NEAR(scene.node_bboxes["top_bar"].width(), 270.0, 1e-3, "stack container width = 10 + 100 + 30 + 120 + 10 = 270");

    ASSERT_NEAR(scene.node_bboxes["g1"].min_x, 32.0, 1e-3, "grid g1 col 0 x = 20 + 12 = 32");
    ASSERT_NEAR(scene.node_bboxes["g2"].min_x, 192.0, 1e-3, "grid g2 col 1 x = 32 + 140 + 20 = 192");
    ASSERT_NEAR(scene.node_bboxes["g3"].min_y, scene.node_bboxes["g1"].max_y + 16.0, 1e-3, "grid g3 row 1 y = g1.bottom + gap_y(16)");

    mesh_string_result_t wgsl_res{};
    mesh_status_t st = mesh_compile_wgsl(src, &wgsl_res);
    ASSERT_TRUE(st == MESH_OK, "mesh_compile_wgsl succeeded");
    std::string wgsl(wgsl_res.data, wgsl_res.length);
    ASSERT_TRUE(wgsl.find("@fragment") != std::string::npos, "WGSL contains @fragment entry point");
    ASSERT_TRUE(wgsl.find("fbm_noise") != std::string::npos, "WGSL contains procedural FBM noise evaluator");
    mesh_free_string_result(&wgsl_res);

    mesh_string_result_t glsl_res{};
    st = mesh_compile_glsl(src, &glsl_res);
    ASSERT_TRUE(st == MESH_OK, "mesh_compile_glsl succeeded");
    std::string glsl(glsl_res.data, glsl_res.length);
    ASSERT_TRUE(glsl.find("#version 450") != std::string::npos, "GLSL contains #version 450 header");
    mesh_free_string_result(&glsl_res);

    mesh::RenderOptions ropts;
    ropts.override_width = 160;
    ropts.override_height = 120;
    std::string gif_path = "/tmp/mesh_wave3_test.gif";
    mesh::Result gr = mesh::render_animation_gif(src, gif_path, 0.4, 5, ropts);
    ASSERT_TRUE(gr.ok, "render_animation_gif succeeded");

    std::ifstream ifs(gif_path, std::ios::binary);
    std::string header(6, '\0');
    ifs.read(&header[0], 6);
    ASSERT_TRUE(header == "GIF89a", "Animated GIF file starts with GIF89a signature");
    std::remove(gif_path.c_str());
}

static void test_mp4_export() {
    if (!mesh::mp4_export_supported()) {
        std::cout << "[skip] MP4 export backend unavailable (FFmpeg not compiled in)\n";
        return;
    }

    const char* src = R"(
        mesh "mp4_export_test" {
          canvas { width: 320; height: 200; background: #0b1220 }
          circle "orb" {
            center: (160, 100)
            radius: oscillate(t, 1.0, 28, 48)
            fill: oklch(0.76, 0.17, 205)
          }
          rect "bar" {
            x: 24
            y: 172
            width: oscillate(t, 0.5, 40, 272)
            height: 8
            radius: 4
            fill: #38bdf8
          }
        }
    )";

    mesh::RenderOptions ropts;
    const std::string mp4_path = "/tmp/mesh_mp4_export_test.mp4";
    mesh::Result r = mesh::render_animation_mp4(src, mp4_path, 0.5, 24, ropts);
    ASSERT_TRUE(r.ok, "render_animation_mp4 succeeded: " + r.error);

    std::ifstream ifs(mp4_path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(ifs.good(), "MP4 output file exists");
    std::streamoff size = ifs.tellg();
    ASSERT_TRUE(size > 2048, "MP4 output is non-trivial in size");
    char header[12] = {};
    ifs.seekg(0, std::ios::beg);
    ifs.read(header, sizeof(header));
    ASSERT_TRUE(std::memcmp(header + 4, "ftyp", 4) == 0, "MP4 file has an ISO-BMFF ftyp marker");
    ifs.close();
    std::remove(mp4_path.c_str());

    const std::string c_path = "/tmp/mesh_mp4_c_api_test.mp4";
    char* err = nullptr;
    mesh_status_t st = mesh_render_mp4_file(src, c_path.c_str(), 0.25, 12, 160, 100, &err);
    ASSERT_TRUE(st == MESH_OK, "mesh_render_mp4_file (C API) succeeded");
    if (err) mesh_free_string(err);
    std::ifstream cs(c_path, std::ios::binary);
    ASSERT_TRUE(cs.good(), "C API MP4 output file exists");
    cs.close();
    std::remove(c_path.c_str());
}

static void test_beta_final_typed_vars_color_ops_and_parametric_shapes() {
    const char* src = R"(
        mesh "beta_final_test" {
          canvas { width: 400; height: 300; background: #0b0e14 }

          let brand = oklch(0.76, 0.17, 200)
          let accent = oklch(0.80, 0.18, 150)
          let blended = mix(@brand, accent, 0.5)
          let pos_a = (100, 120)

          star "s1" {
            center: pos_a
            radius: 40
            inner_radius: 18
            rays: 5
            fill: lighten(blended, 0.10)
            shadow(0, 8, 16, fade(#000000, 0.5))
          }

          hexagon "h1" {
            center: @pos_a + (120, 0)
            radius: 35
            fill: darken(@brand, 0.12)
            glow(12, fade(accent, 0.4))
          }

          ring "r1" {
            center: (320, 120)
            radius: 40
            inner_radius: 22
            fill: blended
          }

          sector "sec1" {
            center: (200, 230)
            radius: 36
            inner_radius: 14
            start_angle: 0
            end_angle: 180
            fill: @accent
          }
        }
    )";

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(src, scene);
    ASSERT_TRUE(pr.ok, "Final Beta scene with typed let vars, color ops, and parametric shapes parses cleanly");
    ASSERT_TRUE(scene.color_variables.count("brand") == 1, "Color variable 'brand' registered");
    ASSERT_TRUE(scene.point_variables.count("pos_a") == 1, "Point variable 'pos_a' registered");
    ASSERT_NEAR(scene.node_bboxes["s1"].center_x(), 100.0, 2.0, "Star s1 centered near x=100");
    ASSERT_NEAR(scene.node_bboxes["h1"].center_x(), 220.0, 2.0, "Hexagon h1 centered near x=220");
    ASSERT_NEAR(scene.node_bboxes["r1"].width(), 80.0, 1e-3, "Ring r1 outer width == 80");

    mesh::BenchmarkStats bstats{};
    mesh::RenderOptions ropts;
    mesh::Result br = mesh::benchmark_scene(src, 2, ropts, bstats);
    ASSERT_TRUE(br.ok, "benchmark_scene succeeded");
    ASSERT_TRUE(bstats.megapixels_per_sec > 0.0, "Benchmark throughput > 0 MPix/s");
    ASSERT_TRUE(bstats.leaf_nodes >= 4, "Benchmark counted leaf nodes");
}

static void test_v1_release_parametric_repeat_transforms_and_easing() {
    const char* src = R"(
        mesh "v1_release_features" {
          canvas { width: 480; height: 320; background: #090d16 }

          let c_start = oklch(0.78, 0.18, 195)
          let c_end   = oklch(0.72, 0.22, 305)
          let base_pt = (40, 220)
          let shifted = base_pt + (20, -10)

          linear_gradient "angled_grad" {
            angle: 135
            stop(0.0, @c_start)
            stop(1.0, @c_end)
          }

          repeat "bars" {
            count: 5
            let bar_h = 20 + i * 15
            rect "bar" {
              x: shifted.x + i * 30
              y: shifted.y - bar_h
              width: 18
              height: bar_h
              radius: 4
              fill: mix(@c_start, @c_end, rep_t)
            }
          }

          stack "nested_stack" {
            x: 40; y: 240; direction: horizontal; gap: 20
            ring "nested_ring" {
              center: (0, 0); radius: 20; inner_radius: 10
              fill: @angled_grad
            }
            rect "status_box" {
              width: 140
              height: ease_out_cubic(t, 0.0, 1.0, 20, 40)
              fill: #1e293b
            }
          }

          text "glow_title" {
            at: (240, 36)
            content: "V1.0 $100 & ~99% {OK}"
            size: 16
            align: center
            fill: #ffffff
            glow(8, fade(@c_start, 0.6))
            shadow(0, 4, 8, fade(#000000, 0.7))
          }
        }
    )";

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(src, scene, 0.5);
    ASSERT_TRUE(pr.ok, "v1.0.0 parametric repeat, dot-access, and nested stack scene parses cleanly");

    // Verify parametric repeat per-iteration evaluation
    ASSERT_TRUE(scene.node_bboxes.count("bar_0") == 1, "bar_0 registered");
    ASSERT_TRUE(scene.node_bboxes.count("bar_4") == 1, "bar_4 registered");
    ASSERT_NEAR(scene.node_bboxes["bar_0"].min_x, 60.0, 1e-3, "bar_0 x == shifted.x (60)");
    ASSERT_NEAR(scene.node_bboxes["bar_4"].min_x, 180.0, 1e-3, "bar_4 x == 60 + 4 * 30 == 180");
    ASSERT_NEAR(scene.node_bboxes["bar_0"].height(), 20.0, 1e-3, "bar_0 height == 20");
    ASSERT_NEAR(scene.node_bboxes["bar_4"].height(), 80.0, 1e-3, "bar_4 height == 20 + 4 * 15 == 80");

    // Verify transform idempotency on Boolean/Group inside stack
    ASSERT_NEAR(scene.node_bboxes["nested_ring"].min_x, 40.0, 1e-3, "nested_ring inside stack min_x == 40 (no double transform)");
    ASSERT_NEAR(scene.node_bboxes["nested_ring"].min_y, 240.0, 1e-3, "nested_ring inside stack min_y == 240 (no double transform)");

    // Verify recompute_scene_Caches idempotency
    mesh::recompute_scene_Caches(scene);
    ASSERT_NEAR(scene.node_bboxes["nested_ring"].min_x, 40.0, 1e-3, "recompute_scene_Caches preserves nested_ring min_x == 40");

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene(scene, img);
    ASSERT_TRUE(rr.ok, "Render v1.0.0 scene with text glow/shadow and extended glyphs");
}

static void test_c_api_and_compilers() {
    ASSERT_TRUE(std::string(mesh_version()) == "1.0.0", "C API version is 1.0.0");

    const char* src = R"(
        mesh "api_test" {
          canvas { width: 128; height: 128; background: #1e293b }
          circle "c" { center: (64, 64); radius: 40; fill: #38bdf8 }
        }
    )";

    mesh_image_buffer_t buf{};
    mesh_status_t st = mesh_render_rgba(src, 0, 0, &buf);
    ASSERT_TRUE(st == MESH_OK, "C API mesh_render_rgba succeeded");
    ASSERT_TRUE(buf.width == 128 && buf.height == 128, "C API buffer 128x128");
    ASSERT_TRUE(buf.rgba_pixels != nullptr, "C API pixel pointer valid");
    mesh_free_image_buffer(&buf);

    mesh_string_result_t svg_res{};
    st = mesh_compile_svg(src, &svg_res);
    ASSERT_TRUE(st == MESH_OK, "C API mesh_compile_svg succeeded");
    ASSERT_TRUE(svg_res.length > 50, "SVG output non-empty");
    mesh_free_string_result(&svg_res);

    mesh_string_result_t html_res{};
    st = mesh_compile_html(src, &html_res);
    ASSERT_TRUE(st == MESH_OK, "C API mesh_compile_html succeeded");
    ASSERT_TRUE(html_res.length > 200, "HTML output non-empty");
    mesh_free_string_result(&html_res);

    const uint8_t* wasm_px = mesh_wasm_render_frame(src, 0.25, 64, 64, 1);
    ASSERT_TRUE(wasm_px != nullptr, "Wasm zero-copy render_frame returned valid linear memory pointer");
    ASSERT_TRUE(mesh_wasm_get_width() == 64 && mesh_wasm_get_height() == 64, "Wasm frame dimensions 64x64");
    const char* wasm_wgsl = mesh_wasm_compile_target(src, "wgsl", 0.25);
    ASSERT_TRUE(wasm_wgsl != nullptr && std::strlen(wasm_wgsl) > 100, "Wasm compile_target('wgsl') succeeded");
}

static mesh::ShapeNode* find_node_recursive(std::vector<mesh::ShapeNode>& nodes, const std::string& id) {
    for (auto& n : nodes) {
        if (n.id == id) return &n;
        if (mesh::ShapeNode* child = find_node_recursive(n.children, id)) return child;
    }
    return nullptr;
}

static void test_real_fonts_and_typography() {
    mesh::FontBackendStatus fst = mesh::FontBackend::instance().status();
    if (!fst.available) {
        std::cout << "[skip] Real font backend unavailable: " << fst.message << "\n";
        return;
    }
    std::ifstream probe("examples/fonts/OpenSans-Regular.ttf", std::ios::binary);
    if (!probe) {
        std::cout << "[skip] examples/fonts/OpenSans-Regular.ttf not found\n";
        return;
    }

    const char* src = R"(
        mesh "font_suite" {
          canvas { width: 800; height: 520; font_family: "Open Sans"; font_fallback: "Noto Sans" }
          font "regular" { family: "Open Sans"; src: "fonts/OpenSans-Regular.ttf" }
          font "bold"    { family: "Open Sans"; src: "fonts/OpenSans-Bold.ttf" }
          font "italic"  { family: "Open Sans"; src: "fonts/OpenSans-Italic.ttf" }
          font "mono"    { family: "DejaVu Sans Mono" }
          linear_gradient "ink" { angle: 0; stop(0.0, #FF7B72); stop(1.0, #79C0FF) }

          text "headline" { at: (40, 52); baseline: true; align: left; content: "Hamburger"; size: 48; font: "regular"; fill: #FFFFFF }
          text "boldline" { at: (40, 112); baseline: true; align: left; content: "Hamburger"; size: 48; font: "bold"; fill: #FFFFFF }
          text "italicline" { at: (40, 172); baseline: true; align: left; content: "Hamburger"; size: 48; font: "italic"; fill: #FFFFFF }
          text "spaced" { at: (40, 232); baseline: true; align: left; content: "Hamburger"; size: 48; font: "regular"; letter_spacing: 4; fill: #FFFFFF }
          text "multiline" { at: (40, 292); baseline: true; align: left; content: "First line\nSecond line"; size: 22; font: "regular"; line_height: 1.5; fill: #FFFFFF }
          text "wrapped" { at: (40, 352); baseline: true; align: left; content: "Automatic wrapping uses shaped advances instead of byte counts."; size: 20; font: "regular"; max_width: 220; wrap: true; fill: #FFFFFF }
          text "centered" { at: (400, 430); align: center; content: "Centered"; size: 24; font: "regular"; fill: #FFFFFF }
          text "rightaligned" { at: (760, 430); align: right; content: "Right"; size: 24; font: "regular"; fill: #FFFFFF }
          text "weighted" { at: (40, 480); baseline: true; align: left; content: "Weighted"; size: 24; font_family: "Open Sans"; weight: 700; fill: #FFFFFF }
          text "unicode" { at: (300, 480); baseline: true; align: left; content: "Hello 日本語 العربية"; size: 20; font: "regular"; font_fallback: "Noto Sans, Noto Sans Arabic, Noto Sans CJK JP"; fill: #FFFFFF }
          text "gradient" { at: (40, 520); baseline: true; align: left; content: "Gradient"; size: 54; font: "regular"; fill: @ink; stroke: #FFFFFF; stroke_width: 0.6 }
        }
    )";

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(src, scene, 0.0, "examples");
    ASSERT_TRUE(pr.ok, "Real-font scene parses and shapes");
    if (!pr.ok) {
        std::cerr << "  font parse error: " << pr.error << "\n";
        return;
    }

    mesh::ShapeNode* regular = find_node_recursive(scene.nodes, "headline");
    mesh::ShapeNode* bold = find_node_recursive(scene.nodes, "boldline");
    mesh::ShapeNode* italic = find_node_recursive(scene.nodes, "italicline");
    mesh::ShapeNode* spaced = find_node_recursive(scene.nodes, "spaced");
    mesh::ShapeNode* multiline = find_node_recursive(scene.nodes, "multiline");
    mesh::ShapeNode* wrapped = find_node_recursive(scene.nodes, "wrapped");
    mesh::ShapeNode* centered = find_node_recursive(scene.nodes, "centered");
    mesh::ShapeNode* right = find_node_recursive(scene.nodes, "rightaligned");
    mesh::ShapeNode* weighted = find_node_recursive(scene.nodes, "weighted");
    mesh::ShapeNode* unicode = find_node_recursive(scene.nodes, "unicode");
    mesh::ShapeNode* gradient = find_node_recursive(scene.nodes, "gradient");

    ASSERT_TRUE(regular && !regular->text_subpaths.empty(), "Regular text has real glyph outlines");
    ASSERT_TRUE(regular && regular->text_width > 100.0, "Regular text advance uses font metrics");
    ASSERT_TRUE(bold && !bold->text_subpaths.empty(), "Bold text has real glyph outlines");
    ASSERT_TRUE(italic && !italic->text_subpaths.empty(), "Italic text has real glyph outlines");
    ASSERT_TRUE(italic && italic->resolved_font_path.find("Italic") != std::string::npos,
                "Italic selection resolves to the italic face");
    ASSERT_TRUE(regular && bold && std::abs(regular->text_width - bold->text_width) > 0.5,
                "Bold advance differs from regular advance");
    ASSERT_TRUE(regular && spaced && spaced->text_width > regular->text_width + 20.0,
                "Letter spacing increases advance width");
    ASSERT_TRUE(multiline && multiline->text_line_count == 2, "Explicit newline produces two lines");
    ASSERT_TRUE(multiline && multiline->text_subpaths.size() > 10, "Multiline text has glyph outlines");
    ASSERT_TRUE(wrapped && wrapped->text_line_count >= 2, "Word wrapping produces multiple lines");
    ASSERT_TRUE(wrapped && wrapped->text_width > 0.0 && wrapped->text_width <= 221.0,
                "Wrapped line width stays inside max_width");
    ASSERT_TRUE(centered && std::abs(centered->bbox.center_x() - 400.0) < 2.0,
                "Center alignment anchors the ink centre at at.x");
    ASSERT_TRUE(right && std::abs(right->bbox.max_x - 760.0) < 3.0,
                "Right alignment anchors the ink right edge at at.x");
    ASSERT_TRUE(weighted && weighted->resolved_font_path.find("Bold") != std::string::npos,
                "Font weight 700 selects the bold face through Fontconfig");
    ASSERT_TRUE(unicode && !unicode->text_subpaths.empty() && unicode->text_width > 100.0,
                "Unicode fallback produces glyph outlines");

    mesh::RasterImage img;
    mesh::Result rr = mesh::render_scene(scene, img);
    ASSERT_TRUE(rr.ok && img.width == 800 && img.height == 520, "Real-font scene renders");
    size_t painted = 0;
    for (size_t i = 3; i < img.rgba.size(); i += 4) {
        if (img.rgba[i] > 0) ++painted;
    }
    ASSERT_TRUE(painted > 1000, "Real glyph pixels are rasterized");
    ASSERT_TRUE(gradient && gradient->style.fill.kind == mesh::PaintKind::LinearGradient,
                "Gradient fill is attached to real text");

    // Text as clip geometry exercises the same SDF path.
    const char* clip_src = R"(
        mesh "text_clip" {
          canvas { width: 320; height: 160; background: #000000 }
          text "word" { at: (20, 130); baseline: true; content: "MESH"; size: 110; font_family: "Open Sans"; fill: #FFFFFF }
          rect { x: 0; y: 0; width: 320; height: 160; fill: #FF3366; clip: @word }
        }
    )";
    mesh::Scene clip_scene;
    mesh::Result cr = mesh::parse_and_evaluate(clip_src, clip_scene);
    ASSERT_TRUE(cr.ok, "Text-as-clip scene parses");
    mesh::RasterImage clip_img;
    mesh::Result cir = mesh::render_scene(clip_scene, clip_img);
    ASSERT_TRUE(cir.ok, "Text-as-clip scene renders");
    size_t clipped = 0;
    for (size_t i = 0; i < clip_img.rgba.size(); i += 4) {
        if (clip_img.rgba[i] > 100 && clip_img.rgba[i + 1] < 100) ++clipped;
    }
    ASSERT_TRUE(clipped > 500, "Glyph-shaped clip produces painted pixels");

    // Text must survive component/use, stack, grid and arbitrary matrix transforms.
    const char* composite_src = R"(
        mesh "text_components" {
          canvas { width: 640; height: 420; background: #101820 }
          font "ui" { family: "Open Sans"; src: "fonts/OpenSans-Regular.ttf"; fallback: "Noto Sans" }
          def "label" {
            text "inner_label" { at: (0, 0); content: "Def"; size: 22; font: "ui"; fill: #FFFFFF }
          }
          use "used_label" { target: @label; at: (30, 40) }
          stack "row" {
            x: 220
            y: 40
            gap: 12
            direction: horizontal
            text { at: (0, 0); content: "One"; size: 20; font: "ui"; fill: #FFFFFF }
            text { at: (0, 0); content: "Two"; size: 20; font: "ui"; fill: #FFFFFF }
          }
          grid "cells" {
            x: 30
            y: 120
            cols: 2
            gap: 14
            text { at: (0, 0); content: "A"; size: 24; font: "ui"; fill: #FFFFFF }
            text { at: (0, 0); content: "B"; size: 24; font: "ui"; fill: #FFFFFF }
            text { at: (0, 0); content: "C"; size: 24; font: "ui"; fill: #FFFFFF }
          }
          text "matrix_text" {
            at: (420, 260)
            content: "Matrix"
            size: 26
            font: "ui"
            fill: #FFFFFF
            matrix: (1, 0, 0, 1, 12, 18)
          }
        }
    )";
    mesh::Scene composite;
    mesh::Result cpr = mesh::parse_and_evaluate(composite_src, composite, 0.0, "examples");
    ASSERT_TRUE(cpr.ok, "Text works inside def/use/stack/grid");
    mesh::ShapeNode* used = find_node_recursive(composite.nodes, "used_label");
    mesh::ShapeNode* row = find_node_recursive(composite.nodes, "row");
    mesh::ShapeNode* cells = find_node_recursive(composite.nodes, "cells");
    mesh::ShapeNode* matrix_text = find_node_recursive(composite.nodes, "matrix_text");
    auto first_text_with_outlines = [](mesh::ShapeNode* n, auto&& self) -> bool {
        if (!n) return false;
        if (n->kind == mesh::ShapeKind::Text && !n->text_subpaths.empty()) return true;
        for (auto& child : n->children) {
            if (self(&child, self)) return true;
        }
        return false;
    };
    ASSERT_TRUE(first_text_with_outlines(used, first_text_with_outlines), "Use component clones real text outlines");
    ASSERT_TRUE(first_text_with_outlines(row, first_text_with_outlines), "Stack lays out real text outlines");
    ASSERT_TRUE(first_text_with_outlines(cells, first_text_with_outlines), "Grid lays out real text outlines");
    ASSERT_TRUE(matrix_text && matrix_text->bbox.valid, "Text with matrix has a valid bbox");
    ASSERT_TRUE(matrix_text && std::abs(matrix_text->transform.tx - 12.0) < 1e-9 &&
                std::abs(matrix_text->transform.ty - 18.0) < 1e-9,
                "matrix(): translation components are preserved");
    mesh::RasterImage composite_img;
    mesh::Result cir2 = mesh::render_scene(composite, composite_img);
    ASSERT_TRUE(cir2.ok, "Text in components/stack/grid/matrix renders");

    // Unknown font ids fail loudly instead of silently falling back.
    mesh::Scene bad;
    mesh::Result br = mesh::parse_and_evaluate(
        "mesh { canvas { width: 10; height: 10 } text { content: \"x\"; font: @missing } }", bad);
    ASSERT_TRUE(!br.ok && br.error.find("Unknown font") != std::string::npos,
                "Unknown font id reports a clear error");

    // Missing explicit font files also fail with a useful message.
    mesh::Result mr = mesh::parse_and_evaluate(
        "mesh { canvas { width: 10; height: 10 } font \"bad\" { family: \"X\"; src: \"/no/such/font.ttf\" } text { content: \"x\"; font: \"bad\" } }",
        bad);
    ASSERT_TRUE(!mr.ok && (mr.error.find("font file not found") != std::string::npos ||
                           mr.error.find("not found") != std::string::npos),
                "Missing font file reports a clear error");

    // Re-layout after programmatic edits.
    if (regular) {
        regular->text_content = "WWWWWW";
        mesh::Result lr = mesh::layout_scene_text(scene);
        ASSERT_TRUE(lr.ok, "layout_scene_text re-lays out programmatic edits");
        ASSERT_TRUE(regular->text_subpaths.size() > 3, "Re-laid text still has outlines");
    }
}

static void test_svg_text_import_export() {
    const char* svg = R"SVG(
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 240">
          <style>
            @font-face { font-family: "Open Sans"; src: url("fonts/OpenSans-Regular.ttf"); }
            .title { font-family: "Open Sans"; font-weight: 700; font-style: italic; letter-spacing: 1.5; }
          </style>
          <g transform="translate(8, 12)">
            <text id="title" class="title" x="32" y="88" font-size="36"
                  fill="#F0F6FC" text-anchor="middle">Imported text</text>
          </g>
        </svg>
    )SVG";

    std::string imported;
    mesh::Result ir = mesh::import_svg_to_mesh(svg, imported);
    ASSERT_TRUE(ir.ok, "SVG text imports");
    ASSERT_TRUE(imported.find("font \"imported_1\"") != std::string::npos,
                "SVG @font-face becomes a font declaration");
    ASSERT_TRUE(imported.find("src: \"fonts/OpenSans-Regular.ttf\"") != std::string::npos,
                "SVG font sources are preserved");
    ASSERT_TRUE(imported.find("content: \"Imported text\"") != std::string::npos,
                "SVG text content is preserved");
    ASSERT_TRUE(imported.find("weight: 700") != std::string::npos,
                "SVG font-weight is preserved");
    ASSERT_TRUE(imported.find("font_style: italic") != std::string::npos,
                "SVG font-style is preserved");
    ASSERT_TRUE(imported.find("align: center") != std::string::npos,
                "SVG text-anchor is preserved");
    ASSERT_TRUE(imported.find("matrix: (1.0000") != std::string::npos ||
                imported.find("matrix: (1.00") != std::string::npos,
                "SVG group transforms become mesh matrices");

    mesh::Scene scene;
    mesh::Result pr = mesh::parse_and_evaluate(imported, scene, 0.0, "examples");
    ASSERT_TRUE(pr.ok, "Imported SVG text compiles");
    mesh::ShapeNode* title = find_node_recursive(scene.nodes, "title");
    ASSERT_TRUE(title && title->kind == mesh::ShapeKind::Text, "Imported SVG text keeps its id");
    ASSERT_TRUE(title && !title->text_subpaths.empty(), "Imported SVG text produces real glyph outlines");
    ASSERT_TRUE(title && std::abs(title->font_size - 36.0) < 1e-6, "Imported SVG font size is exact");
    ASSERT_TRUE(title && std::abs(title->font_weight - 700.0) < 1e-6, "Imported SVG weight is exact");
    ASSERT_TRUE(title && title->font_style == mesh::FontStyle::Italic, "Imported SVG italic is exact");
    ASSERT_TRUE(title && std::abs(title->transform.tx - 8.0) < 1e-6 &&
                std::abs(title->transform.ty - 12.0) < 1e-6,
                "Imported SVG group transforms are preserved");

    std::string exported;
    mesh::Result er = mesh::compile_to_svg(scene, exported);
    ASSERT_TRUE(er.ok, "Text scene exports to SVG");
    ASSERT_TRUE(exported.find("data-mesh-text=\"Imported text\"") != std::string::npos,
                "SVG export tags shaped text with its content");
    ASSERT_TRUE(exported.find("data-mesh-font=\"Open Sans\"") != std::string::npos,
                "SVG export records the resolved font family");
    ASSERT_TRUE(exported.find("<path") != std::string::npos,
                "SVG export emits exact glyph paths rather than browser text metrics");
}

static void test_binary_text_roundtrip() {
    mesh::FontBackendStatus fst = mesh::FontBackend::instance().status();
    if (!fst.available) {
        std::cout << "[skip] Real font backend unavailable for binary text roundtrip\n";
        return;
    }

    const char* src = R"(
        mesh "binary_text" {
          canvas { width: 360; height: 180; background: #0B0F14; font_family: "Open Sans"; font_fallback: "Noto Sans" }
          font "display" { family: "Open Sans"; weight: 700; fallback: "Noto Sans" }
          text "label" {
            at: (24, 92)
            baseline: true
            align: left
            content: "Packed text"
            size: 34
            font: "display"
            letter_spacing: 1.25
            line_height: 1.3
            fill: #FFFFFF
            matrix: (1, 0, 0, 1, 4, 6)
          }
        }
    )";

    mesh_binary_result_t bin{};
    mesh_status_t st = mesh_pack_binary(src, 0.0, &bin);
    ASSERT_TRUE(st == MESH_OK, "Text scene packs to .meshb v5");
    ASSERT_TRUE(bin.length > 64, "Text .meshb payload is non-empty");

    std::vector<uint8_t> bytes(bin.data, bin.data + bin.length);
    mesh::Scene decoded;
    mesh::Result dr = mesh::decode_meshb(bytes, decoded);
    ASSERT_TRUE(dr.ok, ".meshb v5 decodes text and fonts");
    ASSERT_TRUE(decoded.fonts.count("display") == 1, ".meshb preserves the font table");
    mesh::ShapeNode* packed = find_node_recursive(decoded.nodes, "label");
    ASSERT_TRUE(packed && !packed->text_subpaths.empty(), ".meshb preserves shaped text outlines");
    ASSERT_TRUE(packed && packed->text_width > 100.0, ".meshb preserves text metrics");
    ASSERT_TRUE(packed && packed->resolved_font_path.find("Bold") != std::string::npos,
                ".meshb preserves resolved font selection");
    ASSERT_TRUE(packed && std::abs(packed->transform.tx - 4.0) < 1e-9 &&
                std::abs(packed->transform.ty - 6.0) < 1e-9,
                ".meshb preserves text transforms");

    std::string decompiled;
    mesh::Result dc = mesh::decompile_scene_to_mesh(decoded, decompiled);
    ASSERT_TRUE(dc.ok, "Text .meshb decompiles to .mesh");
    ASSERT_TRUE(decompiled.find("font \"display\"") != std::string::npos,
                "Decompiled source emits font declarations");
    ASSERT_TRUE(decompiled.find("Packed text") != std::string::npos,
                "Decompiled source preserves text content");
    ASSERT_TRUE(decompiled.find("font: \"display\"") != std::string::npos,
                "Decompiled source preserves the font id");
    ASSERT_TRUE(decompiled.find("baseline: true") != std::string::npos,
                "Decompiled source preserves baseline anchoring");
    ASSERT_TRUE(decompiled.find("letter_spacing:") != std::string::npos,
                "Decompiled source preserves letter spacing");
    ASSERT_TRUE(decompiled.find("matrix:") != std::string::npos,
                "Decompiled source preserves matrix transforms");

    mesh::Scene reparsed;
    mesh::Result rr = mesh::parse_and_evaluate(decompiled, reparsed, 0.0);
    ASSERT_TRUE(rr.ok, "Decompiled text source reparses");
    mesh::ShapeNode* again = find_node_recursive(reparsed.nodes, "label");
    ASSERT_TRUE(again && !again->text_subpaths.empty(), "Reparsed text has outlines");
    if (packed && again) {
        ASSERT_NEAR(again->bbox.min_x, packed->bbox.min_x, 0.05, "Text bbox min_x survives roundtrip");
        ASSERT_NEAR(again->bbox.min_y, packed->bbox.min_y, 0.05, "Text bbox min_y survives roundtrip");
        ASSERT_NEAR(again->bbox.max_x, packed->bbox.max_x, 0.05, "Text bbox max_x survives roundtrip");
        ASSERT_NEAR(again->bbox.max_y, packed->bbox.max_y, 0.05, "Text bbox max_y survives roundtrip");
    }

    mesh_image_buffer_t out{};
    mesh_status_t rst = mesh_render_binary_rgba(bin.data, bin.length, 0, 0, &out);
    ASSERT_TRUE(rst == MESH_OK && out.width == 360 && out.height == 180,
                "Text .meshb renders without relayout");
    size_t painted = 0;
    if (out.rgba_pixels) {
        for (size_t i = 3; i < static_cast<size_t>(out.width) * out.height * 4; i += 4) {
            if (out.rgba_pixels[i] > 0) ++painted;
        }
    }
    ASSERT_TRUE(painted > 500, "Text .meshb binary rasterizes glyph pixels");
    mesh_free_image_buffer(&out);
    mesh_free_binary_result(&bin);
}

int main() {
    std::cout << "Running Mesh Engine v" << mesh::VERSION << " Production Verification Suite...\n";
    test_oklch_color_science();
    test_relational_layout_and_anchors();
    test_sdf_boolean_csg_and_splines();
    test_wave2_def_use_repeat_clip_and_time();
    test_wave2_binary_meshb_and_svg_import();
    test_wave3_auto_layout_noise_dashes_arrows_and_gpu_shaders();
    test_mp4_export();
    test_beta_final_typed_vars_color_ops_and_parametric_shapes();
    test_v1_release_parametric_repeat_transforms_and_easing();
    test_real_fonts_and_typography();
    test_svg_text_import_export();
    test_binary_text_roundtrip();
    test_c_api_and_compilers();

    std::cout << "----------------------------------------\n";
    std::cout << "Assertions Passed: " << g_passed << "\n";
    std::cout << "Assertions Failed: " << g_failed << "\n";
    std::cout << "----------------------------------------\n";
    return (g_failed == 0) ? 0 : 1;
}
