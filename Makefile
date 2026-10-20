# Makefile for Mesh Vector Graphics Engine (v1.0.0)
# License: Apache-2.0

CXX      ?= g++
EMCXX    ?= em++
PKG_CONFIG ?= pkg-config

# Native font stack. Each dependency is optional: when it is missing the
# engine still builds and falls back to its deterministic built-in font.
FREETYPE_CFLAGS   := $(shell $(PKG_CONFIG) --cflags freetype2 2>/dev/null)
FREETYPE_LIBS     := $(shell $(PKG_CONFIG) --libs freetype2 2>/dev/null)
HARFBUZZ_CFLAGS   := $(shell $(PKG_CONFIG) --cflags harfbuzz 2>/dev/null)
HARFBUZZ_LIBS     := $(shell $(PKG_CONFIG) --libs harfbuzz 2>/dev/null)
FONTCONFIG_CFLAGS := $(shell $(PKG_CONFIG) --cflags fontconfig 2>/dev/null)
FONTCONFIG_LIBS   := $(shell $(PKG_CONFIG) --libs fontconfig 2>/dev/null)

# Native MP4 export. Optional: detected through pkg-config; when FFmpeg is not
# installed the engine still builds and meshc reports that MP4 is unavailable.
FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libswscale 2>/dev/null)
FFMPEG_LIBS   := $(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libswscale 2>/dev/null)

FONT_DEFS := $(if $(FREETYPE_LIBS),-DMESH_HAVE_FREETYPE) $(if $(HARFBUZZ_LIBS),-DMESH_HAVE_HARFBUZZ) $(if $(FONTCONFIG_LIBS),-DMESH_HAVE_FONTCONFIG)
FONT_CFLAGS := $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(FONTCONFIG_CFLAGS)
FONT_LIBS := $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(FONTCONFIG_LIBS)
FFMPEG_DEFS := $(if $(FFMPEG_LIBS),-DMESH_HAVE_FFMPEG)
ifeq ($(FREETYPE_LIBS),)
FONT_DEFS += -DMESH_NO_FONT_CORE
else ifeq ($(HARFBUZZ_LIBS),)
FONT_DEFS += -DMESH_NO_FONT_CORE
endif

CXXFLAGS := -std=c++20 -O3 -Wall -Wextra -pedantic -pthread -Iinclude $(FONT_CFLAGS) $(FONT_DEFS) $(FFMPEG_CFLAGS) $(FFMPEG_DEFS) -MMD -MP
LDFLAGS  := -lm -pthread $(FONT_LIBS) $(FFMPEG_LIBS)

LIB_SRCS := src/lexer_parser.cpp src/rasterizer.cpp src/backends.cpp src/font.cpp src/text_layout.cpp src/mp4.cpp
LIB_OBJS := $(LIB_SRCS:.cpp=.o)
LIB_DEPS := $(LIB_OBJS:.o=.d)

.PHONY: all clean test wasm examples

all: build/meshc build/libmesh.a


src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libmesh.a: $(LIB_OBJS)
	@mkdir -p build
	$(AR) rcs $@ $(LIB_OBJS)

build/meshc: src/main.cpp build/libmesh.a include/mesh.hpp include/mesh_font.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) src/main.cpp build/libmesh.a -o $@ $(LDFLAGS)

build/test_suite: tests/test_suite.cpp build/libmesh.a include/mesh.hpp include/mesh_font.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/test_suite.cpp build/libmesh.a -o $@ $(LDFLAGS)

# Full WebAssembly Build (Emscripten em++ when available, or standalone Wasm kernel builder)
wasm:
	@mkdir -p web
	@if command -v $(EMCXX) >/dev/null 2>&1; then \
		$(EMCXX) -std=c++20 -O3 -Iinclude $(LIB_SRCS) \
			-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 \
			-s EXPORTED_FUNCTIONS='["_mesh_wasm_version","_mesh_wasm_alloc","_mesh_wasm_free","_mesh_wasm_render_frame","_mesh_wasm_compile_target","_mesh_wasm_get_width","_mesh_wasm_get_height","_mesh_wasm_get_last_error"]' \
			-s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","UTF8ToString","stringToUTF8","HEAPU8"]' \
			-o web/mesh_wasm.js; \
		echo "Built Emscripten WebAssembly bundle: web/mesh_wasm.js + web/mesh_wasm.wasm"; \
	else \
		python3 scripts/build_wasm_kernel.py web/mesh_kernel.wasm; \
		echo "Built standalone WebAssembly SDF/Pixel kernel: web/mesh_kernel.wasm"; \
	fi

test: build/test_suite
	./build/test_suite

examples: build/meshc
	@mkdir -p build/examples
	./build/meshc render examples/01_aurora_card.mesh -o build/examples/01_aurora_card.png --aa 4
	./build/meshc render examples/02_ai_topology_graph.mesh -o build/examples/02_ai_topology_graph.png --aa 4
	./build/meshc render examples/03_sdf_metaballs_mesh.mesh -o build/examples/03_sdf_metaballs_mesh.png --aa 4
	./build/meshc render examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.png --time 0.35 --aa 4
	./build/meshc animate examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.gif --duration 2.0 --fps 25 --width 800 --height 480
	./build/meshc compile examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.wgsl --target wgsl --time 0.35
	./build/meshc compile examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.glsl --target glsl --time 0.35
	./build/meshc compile examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.html --target html --time 0.35
	./build/meshc pack examples/04_x_neural_bento_widget.mesh -o build/examples/04_x_neural_bento_widget.meshb --time 0.35
	./build/meshc bench examples/04_x_neural_bento_widget.mesh --iterations 3 --aa 1
	./build/meshc render examples/05_win95_desktop.mesh -o build/examples/05_win95_desktop.png --aa 4 --time 0.35
	./build/meshc render examples/06_win8_start.mesh -o build/examples/06_win8_start.png --aa 4 --time 0.35
	./build/meshc render examples/07_aero_glass.mesh -o build/examples/07_aero_glass.png --aa 4 --time 0.35
	./build/meshc render examples/08_win11_fluent.mesh -o build/examples/08_win11_fluent.png --aa 4 --time 0.35
	./build/meshc render examples/09_typography_specimen.mesh -o build/examples/09_typography_specimen.png --aa 4

clean:
	rm -f src/lexer_parser.o src/rasterizer.o src/backends.o src/font.o src/text_layout.o src/mp4.o src/wasm_bindings.o $(LIB_DEPS) build/meshc build/libmesh.a build/test_suite

-include $(LIB_DEPS)
