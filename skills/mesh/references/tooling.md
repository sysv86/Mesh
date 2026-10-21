# Mesh tooling and verification

Run these commands from the Mesh repository root after `make -j$(nproc)`.

## CLI

```bash
./build/meshc validate scene.mesh
./build/meshc inspect scene.mesh
./build/meshc inspect scene.mesh --json
./build/meshc render scene.mesh -o scene.png --aa 4
./build/meshc render scene.mesh -o scene.png --aa 1 --time 2.4
./build/meshc animate scene.mesh -o scene.mp4 --duration 6 --fps 30
./build/meshc animate scene.mesh -o scene.gif --duration 3 --fps 20
./build/meshc animate scene.mesh -o sheet.png --duration 3 --fps 20
./build/meshc compile scene.mesh --target svg -o scene.svg
./build/meshc compile scene.mesh --target html -o scene.html
./build/meshc compile scene.mesh --target wgsl -o scene.wgsl
./build/meshc compile scene.mesh --target glsl -o scene.glsl
./build/meshc pack scene.mesh -o scene.meshb
./build/meshc unpack scene.meshb -o restored.mesh
./build/meshc import drawing.svg -o drawing.mesh
./build/meshc fonts --json
```

`render` and `compile` select a still by time; `animate` produces a sequence.
`--width`/`--height` override canvas dimensions. `--aa 1` or `4` selects SSAA.
`--threads N` controls tile workers.

## Recommended authoring loop

1. Write the smallest scene that proves the composition.
2. `validate`; fix parser errors before adding detail.
3. `render --aa 1` at the target time.
4. Inspect the PNG at full size; fix clipping, overlap and contrast.
5. Add animation and render `--aa 4` or MP4.
6. Validate once more before returning the file.

A render success does not guarantee a good image. Always inspect the output.

## Binary format

`.meshb` is a compiled, CRC32-protected binary. Use:

```bash
./build/meshc pack source.mesh -o output.meshb
./build/meshc render output.meshb -o output.png
./build/meshc unpack output.meshb -o restored.mesh
```

Version 5 stores text fields, shaped outlines, fonts, metrics and the base
directory. Embedded outlines mean a packed file can render without installed
fonts. `unpack` is for inspection/recovery; edit the `.mesh` source, not the
binary.

## Video export

`meshc animate -o scene.mp4` uses the native optional FFmpeg backend:

- H.264 High, `yuv420p`, CRF 18, preset `medium`;
- no audio;
- `+faststart`;
- even dimensions required (odd sizes are padded);
- no external `ffmpeg` executable is invoked.

If the build lacks FFmpeg, `mesh::mp4_export_supported()` is false and the
command returns a clear error. PNG, SVG and GIF export still work.

## Tests and CI

```bash
make test
make examples
make wasm
```

`make test` builds `build/test_suite` and runs the production verification
suite. CMake is supported:

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j"$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

The GitHub Actions workflow is `.github/workflows/ci.yml`. It runs Make build +
tests, CMake build + CTest, and a short MP4 export smoke test on every push and
pull request targeting `main`.

## Skills

The repository ships Agent Skills under `skills/`:

- `skills/mesh/` — core `.mesh` authoring.
- `skills/mesh-typography/` — fonts, text layout and text as geometry.
- `skills/mesh-motion/` — animation and video export.

Validate the bundled examples when changing the grammar or skill examples:

```bash
for f in skills/mesh/examples/*.mesh; do ./build/meshc validate "$f"; done
```
