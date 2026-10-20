// Copyright 2026 The Mesh Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#ifndef MESH_C_API_H
#define MESH_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mesh_status_t {
    MESH_OK = 0,
    MESH_ERR_SYNTAX = 1,
    MESH_ERR_EVAL = 2,
    MESH_ERR_IO = 3,
    MESH_ERR_INVALID_ARG = 4,
    MESH_ERR_CORRUPT_BINARY = 5
} mesh_status_t;

typedef struct mesh_image_buffer_t {
    uint8_t* rgba_pixels;
    uint32_t width;
    uint32_t height;
    char* error_message;
} mesh_image_buffer_t;

typedef struct mesh_string_result_t {
    char* data;
    size_t length;
    char* error_message;
} mesh_string_result_t;

typedef struct mesh_binary_result_t {
    uint8_t* data;
    size_t length;
    char* error_message;
} mesh_binary_result_t;

// Returns the semantic version string of libmesh ("1.0.0").
const char* mesh_version(void);

// Parses, evaluates, and rasterizes a .mesh source string directly into an 8-bit RGBA pixel buffer.
mesh_status_t mesh_render_rgba(
    const char* mesh_source,
    uint32_t override_width,
    uint32_t override_height,
    mesh_image_buffer_t* out_buffer
);

// Renders a .mesh source string at a specific animation timestamp `time_sec` with SSAA and tile multithreading.
mesh_status_t mesh_render_rgba_at_time(
    const char* mesh_source,
    double time_sec,
    uint32_t override_width,
    uint32_t override_height,
    int ssaa_samples,
    int num_threads,
    mesh_image_buffer_t* out_buffer
);

// Parses, evaluates, and writes a lossless compressed PNG file directly from a .mesh source string.
mesh_status_t mesh_render_png_file(
    const char* mesh_source,
    const char* output_png_path,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
);

// Renders a multi-frame animation from a .mesh source string directly into an animated GIF89a file.
mesh_status_t mesh_render_gif_file(
    const char* mesh_source,
    const char* output_gif_path,
    double duration_sec,
    int fps,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
);

// Renders a multi-frame animation from a .mesh source string directly into an H.264/MP4 file.
// Requires a Mesh build with the optional FFmpeg backend; otherwise returns an error.
mesh_status_t mesh_render_mp4_file(
    const char* mesh_source,
    const char* output_mp4_path,
    double duration_sec,
    int fps,
    uint32_t override_width,
    uint32_t override_height,
    char** out_error_message
);

// Compiles a .mesh source string into a compact binary .meshb wire buffer (magic "MSHB" + CRC32).
mesh_status_t mesh_pack_binary(
    const char* mesh_source,
    double time_sec,
    mesh_binary_result_t* out_binary
);

// Renders a binary .meshb buffer directly into an 8-bit RGBA pixel buffer without text parsing.
mesh_status_t mesh_render_binary_rgba(
    const uint8_t* meshb_data,
    size_t meshb_length,
    uint32_t override_width,
    uint32_t override_height,
    mesh_image_buffer_t* out_buffer
);

// Imports a legacy SVG 1.1 XML string and converts it into clean .mesh declarative syntax.
mesh_status_t mesh_import_svg(
    const char* svg_xml_source,
    mesh_string_result_t* out_mesh_source
);

// Compiles a .mesh source string into a standalone interactive WebGPU WGSL + HTML5 Canvas2D runtime bundle.
mesh_status_t mesh_compile_html(
    const char* mesh_source,
    mesh_string_result_t* out_html
);

// Compiles a .mesh source string into a backwards-compatible SVG 1.1 document.
mesh_status_t mesh_compile_svg(
    const char* mesh_source,
    mesh_string_result_t* out_svg
);

// Compiles a .mesh source string directly into a standalone WebGPU WGSL shader module.
mesh_status_t mesh_compile_wgsl(
    const char* mesh_source,
    mesh_string_result_t* out_wgsl
);

// Compiles a .mesh source string directly into a standalone Vulkan/OpenGL GLSL 450 fragment shader.
mesh_status_t mesh_compile_glsl(
    const char* mesh_source,
    mesh_string_result_t* out_glsl
);

// Formats a .mesh source string into canonical indentation and syntax.
mesh_status_t mesh_format_source(
    const char* mesh_source,
    mesh_string_result_t* out_formatted
);

// Frees memory allocated inside mesh_image_buffer_t.
void mesh_free_image_buffer(mesh_image_buffer_t* buffer);

// Frees memory allocated inside mesh_string_result_t.
void mesh_free_string_result(mesh_string_result_t* result);

// Frees memory allocated inside mesh_binary_result_t.
void mesh_free_binary_result(mesh_binary_result_t* result);

// Frees a standalone error string.
void mesh_free_string(char* str);


// WebAssembly Zero-Copy Linear Memory & Shader Compiler Exports (v1.0.0)
const char* mesh_wasm_version(void);
uint8_t* mesh_wasm_alloc(size_t size);
void mesh_wasm_free(void* ptr);
const char* mesh_wasm_get_last_error(void);
uint32_t mesh_wasm_get_width(void);
uint32_t mesh_wasm_get_height(void);
const uint8_t* mesh_wasm_render_frame(
    const char* mesh_source,
    double time_sec,
    uint32_t override_width,
    uint32_t override_height,
    int ssaa_samples
);
const char* mesh_wasm_compile_target(
    const char* mesh_source,
    const char* target_name,
    double time_sec
);

#ifdef __cplusplus
}
#endif

#endif // MESH_C_API_H
