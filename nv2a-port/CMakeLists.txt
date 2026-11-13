# nv2a-port: xemu's NV2A GPU (PFIFO + PGRAPH, Vulkan renderer) as a static
# library for statically recompiled Xbox titles. See README.md.
cmake_minimum_required(VERSION 3.20)
project(nv2a_xemu C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(NV2A ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/xbox/nv2a)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

option(NV2A_VULKAN "Build the Vulkan renderer (otherwise null renderer only)" ON)

# glslang: GLSL -> SPIR-V at run time (xemu generates every shader).
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
if(NV2A_VULKAN)
    add_subdirectory(${TP}/glslang ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
endif()

if(NV2A_VULKAN)
    file(GLOB NV2A_VK_SRC ${NV2A}/pgraph/vk/*.c)
    set(NV2A_VK_DEPS ${TP}/volk/volk.c ${TP}/SPIRV-Reflect/spirv_reflect.c shim/vma_impl.cpp)
else()
    set(NV2A_VK_SRC "")
    set(NV2A_VK_DEPS "")
endif()
file(GLOB NV2A_VSH_SRC ${TP}/nv2a_vsh_cpu/src/*.c)
file(GLOB NV2A_GLSL_SRC ${NV2A}/pgraph/glsl/*.c)
add_library(nv2a_xemu STATIC
    ${NV2A}/nv2a_host.c
    ${NV2A}/pbus.c ${NV2A}/pcrtc.c ${NV2A}/pfb.c ${NV2A}/pfifo.c ${NV2A}/pmc.c
    ${NV2A}/pramdac.c ${NV2A}/prmcio.c ${NV2A}/prmdio.c ${NV2A}/prmvio.c
    ${NV2A}/ptimer.c ${NV2A}/pvideo.c ${NV2A}/user.c ${NV2A}/debug.c ${NV2A}/stubs.c
    ${NV2A}/pgraph/pgraph.c ${NV2A}/pgraph/profile.c ${NV2A}/pgraph/rdi.c
    ${NV2A}/pgraph/s3tc.c ${NV2A}/pgraph/swizzle.c ${NV2A}/pgraph/texture.c
    ${NV2A}/pgraph/vertex.c ${NV2A}/pgraph/null/renderer.c
    ${NV2A_GLSL_SRC} ${NV2A_VK_SRC}
    ${NV2A_VSH_SRC}
    ${NV2A_VK_DEPS}
    shim/nv2a_shim.c shim/fast-hash.c)
target_include_directories(nv2a_xemu PUBLIC ${NV2A})
target_include_directories(nv2a_xemu PRIVATE
    shim/include src ${NV2A}/pgraph ${TP}/nv2a_vsh_cpu/src ${TP}/volk
    ${TP}/VulkanMemoryAllocator/include ${TP}/SPIRV-Reflect ${TP}/glslang ${TP}/xxhash)
target_compile_definitions(nv2a_xemu PRIVATE VK_NO_PROTOTYPES XXH_INLINE_ALL)
if(NV2A_VULKAN)
    target_compile_definitions(nv2a_xemu PRIVATE NV2A_HAVE_VULKAN=1 CONFIG_VULKAN=1)
endif()
if(ANDROID)
    target_compile_definitions(nv2a_xemu PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(nv2a_xemu PUBLIC android log)
endif()
target_compile_options(nv2a_xemu PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing>)
if(NV2A_VULKAN)
    target_link_libraries(nv2a_xemu PUBLIC glslang glslang-default-resource-limits SPIRV)
endif()
target_link_libraries(nv2a_xemu PUBLIC ${CMAKE_DL_LIBS} m)
find_package(Threads REQUIRED)
target_link_libraries(nv2a_xemu PUBLIC Threads::Threads)
