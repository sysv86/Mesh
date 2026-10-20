cmake_minimum_required(VERSION 3.16)
project(mesh_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(mesh STATIC
    src/lexer_parser.cpp
    src/rasterizer.cpp
    src/backends.cpp
    src/font.cpp
    src/text_layout.cpp
    src/mp4.cpp
)
target_include_directories(mesh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mesh PUBLIC Threads::Threads m)

# Native font stack: optional, so the zero-dependency engine still builds.
if(NOT EMSCRIPTEN)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FREETYPE2 QUIET freetype2)
        pkg_check_modules(HARFBUZZ QUIET harfbuzz)
        pkg_check_modules(FONTCONFIG QUIET fontconfig)
        pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale)

        if(FREETYPE2_FOUND)
            target_compile_definitions(mesh PRIVATE MESH_HAVE_FREETYPE)
            target_include_directories(mesh PRIVATE ${FREETYPE2_INCLUDE_DIRS})
            target_link_libraries(mesh PUBLIC ${FREETYPE2_LIBRARIES})
        endif()
        if(HARFBUZZ_FOUND)
            target_compile_definitions(mesh PRIVATE MESH_HAVE_HARFBUZZ)
            target_include_directories(mesh PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
            target_link_libraries(mesh PUBLIC ${HARFBUZZ_LIBRARIES})
        endif()
        if(FONTCONFIG_FOUND)
            target_compile_definitions(mesh PRIVATE MESH_HAVE_FONTCONFIG)
            target_include_directories(mesh PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
            target_link_libraries(mesh PUBLIC ${FONTCONFIG_LIBRARIES})
        endif()
        if(FREETYPE2_FOUND AND HARFBUZZ_FOUND)
            message(STATUS "Mesh: real font backend enabled (FreeType/HarfBuzz)")
        else()
            message(STATUS "Mesh: built-in fallback font only (install freetype2/harfbuzz for real fonts)")
        endif()

        if(FFMPEG_FOUND)
            target_compile_definitions(mesh PRIVATE MESH_HAVE_FFMPEG)
            target_include_directories(mesh PRIVATE ${FFMPEG_INCLUDE_DIRS})
            target_link_libraries(mesh PUBLIC ${FFMPEG_LIBRARIES})
            message(STATUS "Mesh: native MP4/H.264 export enabled (FFmpeg/libx264)")
        else()
            message(STATUS "Mesh: MP4 export disabled (install FFmpeg dev libraries for .mp4/.mov output)")
        endif()
    else()
        message(STATUS "Mesh: optional native backends disabled (pkg-config not found)")
    endif()
endif()

if(EMSCRIPTEN)
    message(STATUS "Mesh: MP4 export disabled for Emscripten builds")
endif()

if(NOT (FREETYPE2_FOUND AND HARFBUZZ_FOUND))
    target_compile_definitions(mesh PRIVATE MESH_NO_FONT_CORE)
endif()

if(EMSCRIPTEN)
    add_executable(mesh_wasm src/backends.cpp)
    target_link_libraries(mesh_wasm PRIVATE mesh)
    set_target_properties(mesh_wasm PROPERTIES
        LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_FUNCTIONS=['_mesh_wasm_version','_mesh_wasm_alloc','_mesh_wasm_free','_mesh_wasm_render_frame','_mesh_wasm_compile_target','_mesh_wasm_get_width','_mesh_wasm_get_height','_mesh_wasm_get_last_error']"
    )
else()
    add_executable(meshc src/main.cpp)
    target_link_libraries(meshc PRIVATE mesh Threads::Threads m)

    enable_testing()
    add_executable(mesh_test_suite tests/test_suite.cpp)
    target_link_libraries(mesh_test_suite PRIVATE mesh Threads::Threads m)
    add_test(NAME mesh_verification_suite COMMAND mesh_test_suite)
    set_tests_properties(mesh_verification_suite PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
