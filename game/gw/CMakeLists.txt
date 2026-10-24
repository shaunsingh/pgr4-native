# Geometry Wars: Retro Evolved (PGR4's gw.xex) - ReXGlue recompiled project. native/build.sh stages
# this folder with gw.xex in game-data/ and runs codegen, which writes generated/. It draws through
# ReXGlue's Xenos renderer.

cmake_minimum_required(VERSION 3.25)
project(gw_recompiled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Title launches are Windows processes: build for Windows (native/build.sh cross-compiles).")
endif()

set(PGR4_HOST_REXGLUE "" CACHE FILEPATH "A codegen tool that runs on the build machine")

include(generated/rexglue.cmake)
if(PGR4_HOST_REXGLUE)
    # The SDK package's rexglue.exe is the target's; codegen runs on the build machine.
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${PGR4_HOST_REXGLUE}")
endif()

add_executable(gw_recompiled WIN32 src/main.cpp ../common/title_launch.cpp)
target_include_directories(gw_recompiled PRIVATE ../common)
rexglue_setup_target(gw_recompiled)
target_link_options(gw_recompiled PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/gw_recompiled.map")
