# pgr4_shader_xlate: ReXGlue's Xenos ucode analysis and DXBC shader translator (BSD-3, from
# Xenia), built from the SDK source tree as a static library for the app. In the SDK these
# sources only exist inside the rexgpu-xenos plugin DLL, which exports nothing.
#
#   set(PGR4_REXGLUE_SDK_SRC <SDK source root>)   # default: thirdparty/rexglue-sdk of the game
#   include(<this file>)
#   target_link_libraries(<target> PRIVATE pgr4_shader_xlate)
#
# Requires the rex::runtime and rex::xxhash imported targets (find_package(rexglue)).
if(NOT PGR4_REXGLUE_SDK_SRC)
    set(PGR4_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk")
endif()
set(_pgr4_gfx "${PGR4_REXGLUE_SDK_SRC}/src/graphics")
add_library(pgr4_shader_xlate STATIC
    ${_pgr4_gfx}/format/ucode.cpp
    ${_pgr4_gfx}/pipeline/shader/shader.cpp
    ${_pgr4_gfx}/pipeline/shader/translator.cpp
    ${_pgr4_gfx}/pipeline/shader/translator_disasm.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc_translator.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc_translator_alu.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc_translator_fetch.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc_translator_memexport.cpp
    ${_pgr4_gfx}/pipeline/shader/dxbc_translator_om.cpp
    ${PGR4_REXGLUE_SDK_SRC}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_include_directories(pgr4_shader_xlate PRIVATE "${PGR4_REXGLUE_SDK_SRC}")
# The translator sources define three GPU cvars the plugin DLL registers too; the shim turns the
# app's copies into unregistered storage (see the header).
target_compile_options(pgr4_shader_xlate PRIVATE
    "/FI${CMAKE_CURRENT_LIST_DIR}/sdk_cvar_shim.h" -Wno-unused-variable)
target_link_libraries(pgr4_shader_xlate PUBLIC rex::runtime rex::xxhash)
unset(_pgr4_gfx)
