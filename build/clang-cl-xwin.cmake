# CMake toolchain: Windows x64 binaries cross-compiled on Linux with clang-cl and lld-link, against
# an xwin splat of the MSVC CRT and the Windows SDK (https://github.com/Jake-Shadle/xwin):
#
#   xwin --accept-license --arch x86_64 splat --output <dir>
#   cmake -DCMAKE_TOOLCHAIN_FILE=<this file> -DXWIN_DIR=<dir> ...
#
# The result is the same kind of binary the Windows build host produces with clang-cl and the
# Visual Studio Build Tools (MSVC ABI, dynamic CRT), so it runs under Proton as that one does.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT XWIN_DIR)
    set(XWIN_DIR "$ENV{XWIN_DIR}")
endif()
if(NOT XWIN_DIR OR NOT EXISTS "${XWIN_DIR}/crt/include")
    message(FATAL_ERROR "XWIN_DIR must point at an xwin splat (crt/ and sdk/ inside); got '${XWIN_DIR}'")
endif()
# try_compile projects get a fresh cache; pass the splat location on to them. The splat has no
# debug CRT (msvcrtd.lib), so they build Release.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_ASM_MASM_COMPILER llvm-ml)

# The splat's headers as system headers (-imsvc), so their warnings stay quiet as with MSVC's own.
# CMake reads a toolchain file more than once per configure, so these are set, never appended.
set(_xwin_includes "")
foreach(dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
    string(APPEND _xwin_includes " /imsvc${XWIN_DIR}/${dir}")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_xwin_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_xwin_includes}")

set(_xwin_libs "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "${_xwin_libs}")
endforeach()

# llvm-rc needs the SDK's resource headers (winres.h, winver.h).
set(CMAKE_RC_FLAGS_INIT "-I${XWIN_DIR}/sdk/include/um -I${XWIN_DIR}/sdk/include/shared -I${XWIN_DIR}/crt/include")

# Host programs (codegen tool, python, git) come from Linux; libraries and headers from the targets.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
