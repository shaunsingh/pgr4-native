# Project Gotham Racing 4 (Title Update #2) - ReXGlue recompiled project with the native D3D12
# renderer. scripts/build.sh stages this folder with the game data in game-data/ and runs codegen,
# which writes generated/: the recompiled code and generated/rexglue.cmake, the SDK integration.

cmake_minimum_required(VERSION 3.25)
project(pgr4_recompiled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "The native renderer is D3D12: build for Windows (scripts/build.sh cross-compiles).")
endif()

set(PGR4_NATIVE_SRC "" CACHE PATH "The native renderer's sources (src/ of pgr4-native)")
set(PGR4_REXGLUE_SDK_SRC "" CACHE PATH "The source tree the ReXGlue SDK was built from")
set(PGR4_HOST_REXGLUE "" CACHE FILEPATH "A codegen tool that runs on the build machine")
set(PGR4_FIDELITYFX_SDK "" CACHE PATH "The AMD FidelityFX SDK checkout (scripts/fidelityfx.lock)")
foreach(dir PGR4_NATIVE_SRC PGR4_REXGLUE_SDK_SRC PGR4_FIDELITYFX_SDK)
    if(NOT IS_DIRECTORY "${${dir}}")
        message(FATAL_ERROR "${dir} is not set to a directory")
    endif()
endforeach()

include(generated/rexglue.cmake)
if(PGR4_HOST_REXGLUE)
    # The SDK package's rexglue.exe is the target's; codegen runs on the build machine.
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${PGR4_HOST_REXGLUE}")
endif()

file(GLOB PGR4_NATIVE_SOURCES CONFIGURE_DEPENDS "${PGR4_NATIVE_SRC}/native/*.cpp")
add_executable(pgr4_recompiled WIN32
    src/main.cpp
    src/pgr4_hooks.cpp
    ../common/title_launch.cpp
    "${PGR4_NATIVE_SRC}/pgr4_d3d_trace.cpp"
    ${PGR4_NATIVE_SOURCES}
    # Texture format tables the renderer shares with the Xenos plugin, which exports none of them.
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp"
)
target_include_directories(pgr4_recompiled PRIVATE "${PGR4_NATIVE_SRC}" ../common)
# The upscaler's API (src/native/upscaler.cpp); its DLLs are loaded at run time.
target_include_directories(pgr4_recompiled SYSTEM PRIVATE
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include")
rexglue_setup_target(pgr4_recompiled)
target_link_libraries(pgr4_recompiled PRIVATE d3d12 dxgi dxguid)
include("${PGR4_NATIVE_SRC}/native/shader_xlate.cmake")
target_link_libraries(pgr4_recompiled PRIVATE pgr4_shader_xlate)
# pgr4_recompiled.map: function addresses for profiles of the PE image (scripts/analysis/perf_symbols.py).
target_link_options(pgr4_recompiled PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/pgr4_recompiled.map")
