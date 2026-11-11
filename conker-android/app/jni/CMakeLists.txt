cmake_minimum_required(VERSION 3.20)
project(conker_android C CXX)
set(CMAKE_C_STANDARD 11)

# ── SDL2 (window, EGL/GLES, audio, controllers) ─────────────────────
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL2)
if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 ALIAS SDL2)
endif()
set(SDL2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake-shim CACHE PATH "" FORCE)
# The toolkit's targets include <SDL.h> directly.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include)

# ── xemu's NV2A GPU (Vulkan) — must exist before the toolkit is added ─
add_subdirectory(nv2a-port ${CMAKE_BINARY_DIR}/nv2a-port)

# ── xboxrecomp runtime ─────────────────────────────────────────────
set(XBOXRECOMP_BUILD_D3D8 OFF CACHE BOOL "" FORCE)  # LTCG title: pushbuffer executor
set(XBOXRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(xboxrecomp ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── The recompiled title ───────────────────────────────────────────
set(CONKER ${CMAKE_CURRENT_SOURCE_DIR}/conker)
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS ${CONKER}/src/recomp/gen/*.c)
add_library(main SHARED
    ${CONKER}/src/main.c
    ${CONKER}/src/recomp_manual.c
    ${CONKER}/src/xmv_native.c
    ${CONKER}/src/recomp_extra_seeds.c
    ${RECOMP_GEN_SOURCES})
target_include_directories(main PRIVATE
    ${CONKER}/src ${CONKER}/src/recomp ${CONKER}/src/recomp/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/video
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/input
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/apu
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/nv2a
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp/src/platform)
# Generated code: every lifted function is huge straight-line C. -O1 keeps
# compile time and memory sane; the hot paths get tuned later.
set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-O1;-fno-strict-aliasing;-fwrapv;-Wno-everything")
target_compile_options(main PRIVATE -fno-strict-aliasing -fwrapv)
# libadrenotools (BSD-2-Clause, Billy Laws): loads a custom Vulkan driver
# (Turnip) on Adreno through the system loader, outside the app namespace.
set(ADRENO ${CONKER}/third_party/adrenotools)
add_library(adrenotools_lite STATIC
    ${ADRENO}/src/driver.cpp
    ${ADRENO}/lib/linkernsbypass/android_linker_ns.cpp
    ${ADRENO}/lib/linkernsbypass/elf_soname_patcher.cpp)
target_include_directories(adrenotools_lite PUBLIC ${ADRENO}/include
    PRIVATE ${ADRENO} ${ADRENO}/src ${ADRENO}/lib/linkernsbypass)
target_link_libraries(adrenotools_lite PRIVATE android dl log)
# FFmpeg 7.1 (LGPL, static, trimmed: XMV demuxer + WMV2/ADPCM/WMA decoders)
# for the native XMV movie player; see conker/third_party/ffmpeg.
set(FFMPEG ${CONKER}/third_party/ffmpeg)
target_include_directories(main PRIVATE ${FFMPEG}/include)
set(FFLIB ${FFMPEG}/lib/${ANDROID_ABI})
target_link_libraries(main PRIVATE ${FFLIB}/libavformat.a ${FFLIB}/libavcodec.a
    ${FFLIB}/libswscale.a ${FFLIB}/libswresample.a ${FFLIB}/libavutil.a aaudio z)
# FFmpeg's NEON code addresses its tables pc-relative: keep its symbols local
target_link_options(main PRIVATE
    "-Wl,--exclude-libs,libavformat.a:libavcodec.a:libswscale.a:libswresample.a:libavutil.a")
target_link_libraries(main PRIVATE xboxrecomp nv2a_xemu adrenotools_lite SDL2::SDL2 log android GLESv3 EGL m)
