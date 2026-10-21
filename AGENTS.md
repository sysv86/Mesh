# AGENTS.md

This file is the canonical guide for working on Mesh. It describes the
repository, the build and verification workflow, the architecture, and the
rules that keep changes safe and reviewable.

## What Mesh is

Mesh is a declarative 2D vector graphics engine and compiler. A `.mesh`
document contains primitives, paths, typography, gradients, SDF effects,
groups, components, relational anchors and time-based animation. The native
engine rasterizes the scene through a tile-binned SDF pipeline. The CLI can
also emit PNG, SVG, HTML, WGSL, GLSL, `.meshb` binaries, sprite sheets, GIF and
native H.264 MP4/MOV video.

The production version is `1.0.0`, licensed under Apache-2.0.

## First steps

```bash
git status --short
make -j"$(nproc)"
make test
```

The build produces:

- `build/meshc` — CLI compiler, renderer and exporter.
- `build/libmesh.a` — static C++ library.
- `build/test_suite` — production verification suite.

CMake is supported:

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

## Repository map

| Path | Purpose |
| :--- | :--- |
| `src/lexer_parser.cpp` | Lexer, parser, expression evaluator, scene construction |
| `src/rasterizer.cpp` | SDF rasterizer, CSS/PNG/GIF/sprite-sheet export helpers |
| `src/text_layout.cpp` | Font resolution, scene-wide text layout and re-layout |
| `src/font.cpp` | FreeType/HarfBuzz/Fontconfig backend and fallback font |
| `src/mp4.cpp` | Optional native H.264 MP4/MOV exporter |
| `src/backends.cpp` | `.meshb`, SVG import/export, HTML, C API wrappers |
| `src/main.cpp` | CLI commands and option parsing |
| `include/mesh.hpp` | Public C++ scene, renderer and exporter API |
| `include/mesh_font.hpp` | Font request, layout and metrics API |
| `include/mesh_c_api.h` | Stable C ABI |
| `tests/test_suite.cpp` | End-to-end regression suite |
| `examples/` | Showcase documents, bundled fonts and goose examples |
| `skills/` | Portable Agent Skills for Mesh authoring, typography and motion |
| `web/index.html` | Browser parser, Canvas2D preview and preset gallery |
| `scripts/` | Standalone WASM kernel builder and project scripts |

## Working rules

1. **English only.** Documentation, code comments, CLI text, examples and web
   UI strings must be English. Keep Unicode only where a document is
   intentionally demonstrating glyph coverage or multilingual text.
2. **Make the smallest connected change.** Update the implementation, public
   API, C API, CLI, tests and docs together when behavior changes. Avoid broad
   refactors that are not required by the task.
3. **Preserve optional dependencies.** FreeType, HarfBuzz, Fontconfig and
   FFmpeg are optional. A zero-dependency build must still compile, link and
   render with the built-in fallback font. MP4 must fail with an actionable
   error when FFmpeg is absent.
4. **No shell-outs for core features.** MP4 encoding links the FFmpeg C
   libraries directly; it does not invoke the `ffmpeg` executable.
5. **No generated noise.** Do not commit `build/`, object files, dependency
   files, caches, temporary renders or `.meshb` output unless a tracked
   example explicitly requires it. Generated example media is allowed only
   when it is part of the documented showcase.
6. **Keep the public surface coherent.** If you add a C++ exporter, expose the
   corresponding C API wrapper and CLI path when appropriate. Keep names,
   defaults and error messages consistent with existing exporters.
7. **Determinism matters.** Animation exporters render timestamps as
   `frame / fps`; do not introduce wall-clock time into rendering.
8. **Warnings are defects.** Build with `-Wall -Wextra -pedantic` and leave the
   tree warning-free.

## CLI workflow

```bash
./build/meshc validate input.mesh
./build/meshc inspect input.mesh --json
./build/meshc render input.mesh -o out.png --aa 4 --time 0.5
./build/meshc animate input.mesh -o out.mp4 --duration 3 --fps 30
./build/meshc pack input.mesh -o out.meshb
./build/meshc unpack out.meshb -o restored.mesh
./build/meshc compile input.mesh --target svg  -o out.svg
./build/meshc compile input.mesh --target html -o out.html
./build/meshc import drawing.svg -o drawing.mesh
./build/meshc fonts --json
```

`meshc animate` is extension-driven:

- `.mp4` / `.mov` → native H.264 video.
- `.gif` → GIF89a.
- `.png` → horizontal sprite sheet.

For MP4, the defaults are H.264 High profile, `yuv420p`, CRF 18, preset
`medium`, no audio, `+faststart`, and the scene name as the media title.
`yuv420p` requires even dimensions; odd canvases are rounded up by one pixel
and padded with the scene background.

## Architecture notes

### Scene and rendering

The parser produces a `Scene` of `ShapeNode`s. Paint servers, reusable defs,
fonts, variables, relational anchors, boolean groups and layout containers are
resolved before rendering. The rasterizer evaluates signed distance fields for
primitives, paths, booleans, strokes, fills, shadows, glows, grain and text
outlines. The same scene data drives CPU rendering, SVG/HTML export, shader
codegen and binary serialization.

### Text and fonts

Text is shaped at parse time and converted to real glyph outlines stored in
`ShapeNode::text_subpaths`. Those outlines then use the same SDF, paint,
stroke, clip, blend, transform and effect pipeline as ordinary geometry.

- `font "id" { ... }` declarations support local `.ttf`, `.otf`, `.ttc`/`.otc`
  files and Fontconfig family matching.
- Text properties include `font`, `font_family`, `font_src`, `font_fallback`,
  `font_style`, `font_stretch`, `font_features`, `font_language`, `size`,
  `weight`, `align`, `letter_spacing`, `word_spacing`, `line_height`,
  `max_width`, `wrap` and `baseline`.
- `at:` is the visual centre of the ink by default. `baseline: true` opts into
  SVG-style first-baseline anchoring.
- Relative font paths resolve against the directory containing the `.mesh`
  document. Declare fonts before the text that references them.
- The built-in deterministic vector font is the fallback when FreeType and
  HarfBuzz are unavailable.

### Binary format

`.meshb` is a CRC32-protected binary wire format. Version 5 stores text fields,
shaped outlines, font tables, metrics and the document base directory. The
reader accepts older versions. Rendering a `.meshb` must not depend on installed
fonts when shaped text outlines are present. `meshc unpack` emits editable
`.mesh` source.

### MP4

`src/mp4.cpp` is compiled with `MESH_HAVE_FFMPEG`. It renders frames through
`parse_and_evaluate` + `render_scene_ex`, composites RGBA over the scene
background, converts to YUV420P with libswscale, encodes H.264 with libx264,
and muxes through libavformat. `mesh::render_animation_mp4(...)` and
`mesh_render_mp4_file(...)` are the public entry points.

If FFmpeg development libraries are unavailable, `mp4_export_supported()`
returns `false`, the MP4 exporter returns a clear error, and all other formats
continue to work.

### SVG, HTML, shaders and C API

- SVG export emits exact glyph paths and shape geometry, preserving paints,
  transforms, opacity and effects where possible.
- SVG import supports `<text>`/`<tspan>`, CSS classes, local `@font-face`,
  transforms, anchors and baseline settings. Inline non-positioned tspans are
  merged into one text node.
- HTML export produces a standalone animated document.
- WGSL/GLSL targets emit SDF shader code; text is not represented in shader
  output.
- The C API mirrors the supported rendering, PNG, GIF, MP4 and binary paths.

## Verification

Run the narrowest relevant check first, then the full suite when risk warrants
it.

```bash
make test
make examples
make wasm
```

Additional checks:

- CMake configure/build/CTest in a throwaway directory.
- No-font build: `-DMESH_NO_FONT_CORE`; `meshc fonts` must report no backend
  and rendering must use the fallback font.
- No-FFmpeg build: `mp4_export_supported()` is false and `.mp4` requests fail
  cleanly without leaving a file.
- ASAN/UBSAN for rasterizer, text and exporter changes.
- `ffprobe` for MP4: H.264, expected dimensions/fps/duration, no audio and
  `moov` before `mdat`.
- `meshc pack` → render `.meshb` → `unpack` → validate for binary changes.

## Agent skills

The repository publishes portable Agent Skills in `skills/`:

- `skills/mesh/SKILL.md` — core `.mesh` language and authoring workflow.
- `skills/mesh-typography/SKILL.md` — font declarations, text layout and text as geometry.
- `skills/mesh-motion/SKILL.md` — time expressions, animation and video export.

Each skill has YAML frontmatter with `name` and `description`; the description
is the routing text. Keep `SKILL.md` lean and put detailed tables in
`references/`. Every bundled `.mesh` example must validate:

```bash
bash skills/mesh/scripts/validate-examples.sh
```

Do not add a new skill for a narrow one-off task. Update an existing skill or
its references instead.

## Examples and assets

`examples/01`–`09` cover UI, topology, SDF metaballs, bento layouts, Windows
95/8/Aero/11 and typography. `examples/fonts/` contains the bundled Open Sans
faces and their OFL license.

`examples/10_goose_mesh.mesh` and `examples/11_goose_svg.svg` are the
goose-and-MacBooks comparison pieces, with matching MP4 renders and a
comparison image. Keep their source and media together when updating them.

## Before publishing

- Run `make test` and the relevant build/export checks.
- Remove stale generated files and verify `.gitignore` coverage.
- Review `git diff --stat`, `git diff --check` and staged paths.
- Do not commit personal email addresses; use the repository's configured
  noreply identity.
- Publish only when the user explicitly asks.
