# Offline shader translation oracle (see main.cpp). Build on Windows:
#   cmake -S . -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl
#         -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH=<sdk-install>
#         -DPGR4_REXGLUE_SDK_SRC=<ReXGlue source tree>
cmake_minimum_required(VERSION 3.25)
project(shader_xlate_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue REQUIRED)
set(PGR4_NATIVE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
include("${PGR4_NATIVE_SRC}/native/shader_xlate.cmake")

add_executable(shader_xlate_test
    main.cpp
    ${PGR4_NATIVE_SRC}/native/shader_translate.cpp
    ${PGR4_NATIVE_SRC}/native/shader_cache.cpp
    ${PGR4_NATIVE_SRC}/native/geometry_shader.cpp
)
target_include_directories(shader_xlate_test PRIVATE "${PGR4_NATIVE_SRC}")
target_link_libraries(shader_xlate_test PRIVATE pgr4_shader_xlate d3d12 dxgi)
add_custom_command(TARGET shader_xlate_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:shader_xlate_test> $<TARGET_FILE_DIR:shader_xlate_test>
    COMMAND_EXPAND_LISTS)
