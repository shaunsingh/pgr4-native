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
    src/pgr4_lua.cpp
    src/pgr4_settings.cpp
    ../common/title_launch.cpp
    "${PGR4_NATIVE_SRC}/pgr4_d3d_trace.cpp"
    ${PGR4_NATIVE_SOURCES}
    # Texture format tables the renderer shares with the Xenos plugin, which exports none of them.
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
    "${PGR4_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp"
)
target_include_directories(pgr4_recompiled PRIVATE "${PGR4_NATIVE_SRC}" ../common)
# The port's UI scripts (ui/, in Fennel), compiled to Lua for the game's Lua 5.1 and embedded by
# pgr4_lua.cpp. The compiler accepts only the globals listed here: the game's UI functions and
# tables the scripts use, the screen functions they define, and Lua's own they call, so a
# misspelt name fails the build.
set(PGR4_FENNEL "" CACHE FILEPATH "The Fennel compiler (scripts/fennel.lock)")
if(NOT EXISTS "${PGR4_FENNEL}")
    message(FATAL_ERROR "PGR4_FENNEL is not set to the Fennel compiler")
endif()
find_program(PGR4_LUA NAMES lua luajit lua5.4 lua5.3 lua5.1 REQUIRED)
set(PGR4_UI_GLOBALS
    # Lua 5.1
    error ipairs math pairs pcall table
    # The game's UI
    CustomPopup GUIBank GothamSystem UIButtons UIEnums UIGlobals UIScreen UIText
    numberOfViewports
    GetDefaultFrame MoveScrollBarV PlayMenuBack PlayMenuMove PlayMenuNext SetClipToFrame
    SetupCustomPopup SetupFadedBackground SetupFadePanel SetupFrameScrollBar SetupFrameTitle
    SetupGroupEnd SetupInfoText SetupIngameSlideGroup SetupScreenTimers SetupSlideMenuInfo
    SetupVariableSpinner StoreMenuSelection
    # A script's own, which a patch wraps
    AdjustPositionAndSizeForViewports GetViewportDetails GUI Init MessageUpdate
    RedoScreenGlobals
    # pgr4_lua.cpp
    Pgr4)
string(JOIN "," PGR4_UI_GLOBALS_ARG ${PGR4_UI_GLOBALS})
file(GLOB_RECURSE PGR4_UI_SCRIPTS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/ui" ui/*.fnl)
set(PGR4_UI_LUA "")
foreach(fnl IN LISTS PGR4_UI_SCRIPTS)
    string(REGEX REPLACE "\\.fnl$" ".lua" lua "${CMAKE_CURRENT_BINARY_DIR}/pgr4_ui/${fnl}")
    add_custom_command(OUTPUT "${lua}"
        COMMAND "${CMAKE_COMMAND}" "-DLUA=${PGR4_LUA}" "-DFENNEL=${PGR4_FENNEL}"
            "-DGLOBALS=${PGR4_UI_GLOBALS_ARG}" "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/ui/${fnl}"
            "-DOUT=${lua}" -P "${CMAKE_CURRENT_SOURCE_DIR}/ui/fennel.cmake"
        DEPENDS "ui/${fnl}" ui/fennel.cmake "${PGR4_FENNEL}"
        COMMENT "Compiling ui/${fnl}"
        VERBATIM)
    list(APPEND PGR4_UI_LUA "${lua}")
endforeach()
target_sources(pgr4_recompiled PRIVATE ${PGR4_UI_LUA})
# #embed searches beside the file, then the embed directories, not the include path; and with -MD clang embeds a file it
# cannot find as empty instead of failing (pgr4_lua.cpp asserts each script is not).
set_source_files_properties(src/pgr4_lua.cpp PROPERTIES OBJECT_DEPENDS "${PGR4_UI_LUA}"
    COMPILE_OPTIONS "/clang:--embed-dir=${CMAKE_CURRENT_BINARY_DIR}")
# The FidelityFX API of the upscaler and the frame generation (src/native/fidelityfx.h); its DLLs
# are loaded at run time.
target_include_directories(pgr4_recompiled SYSTEM PRIVATE
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include"
    "${PGR4_FIDELITYFX_SDK}/Kits/FidelityFX/framegeneration/include")
rexglue_setup_target(pgr4_recompiled)
# d3d12.dll loads at the first D3D12 call, as in Geometry Wars (whose D3D12 is all the SDK's,
# loaded at run time), not with the executable. OptiScaler (play.sh --fgmod), loaded as dxgi.dll,
# probes the GPU on a thread of its own with a D3D12 device when d3d12.dll is already loaded as
# it starts; vkd3d-proton returns that device to the game's D3D12CreateDevice too (one device per
# adapter), and about one start in four then crashed in vkd3d-proton at the presenter's first
# CreateGraphicsPipelineState.
target_link_libraries(pgr4_recompiled PRIVATE d3d12 dxgi dxguid delayimp)
target_link_options(pgr4_recompiled PRIVATE /DELAYLOAD:d3d12.dll)
include("${PGR4_NATIVE_SRC}/native/shader_xlate.cmake")
target_link_libraries(pgr4_recompiled PRIVATE pgr4_shader_xlate)
# pgr4_recompiled.map: function addresses for profiles of the PE image (scripts/analysis/perf_symbols.py).
target_link_options(pgr4_recompiled PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/pgr4_recompiled.map")
