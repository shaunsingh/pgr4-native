#!/usr/bin/env bash
# Builds the PGR4 native-renderer tree on Linux (written on a Steam Deck), the counterpart of
# build.ps1: the same stages, sources, patches and compiler flags, cross-compiled for Windows x64
# with clang-cl and lld-link against an xwin splat of the MSVC CRT and Windows SDK. The result
# runs under Proton like a build from the Windows host. The MTR build under $root/MTR-PGR4 is
# only read, never modified.
#
#   native/build.sh [--stage prep,sdk,game,install] [--root DIR] [--dest DIR] [--game DIR]
#                   [--jobs N] [--march x86-64-v3] [--mtune znver2] [--force]
#
# Stages, in order (default: all):
#   prep     rsync $root/MTR-PGR4 (source and patched SDK source; no build dirs, no generated
#            code) to $dest/MTR-PGR4, apply rexglue-drawcount.patch, rexglue-native-primary.patch
#            and rexglue-runtime-perf.patch to the SDK copy, copy src/pgr4_d3d_trace.cpp and src/d3d_surface.inc into the game's
#            src/, mirror src/native/ and add both to the game's CMakeLists.txt.
#   sdk      build the codegen tool (rexglue) for this machine into $dest/host-sdk, then
#            cross-build the SDK copy and install it into $dest/sdk-install.
#   game     codegen default.xex with the copy's manifest (MTR's game patches included), build
#            out/pgr4/pgr4_recompiled.exe.
#   install  $dest/run/runtime: exe, rexruntime.dll, rexgpu-xenos.dll.
#
# --march / --mtune set the target CPU of the SDK and the game. The default is the Steam Deck's
# Zen 2: x86-64-v3 brings FMA (the recompiled code's thousands of std::fma calls become one
# instruction instead of a call into the CRT's software fma, Wine's under Proton), AVX2 for the
# SIMD emulation and MOVBE for the byte-swapped guest loads. --march x86-64-v2 builds for the
# older CPUs build.ps1 targets. A tree configured with other flags is reconfigured (and so
# rebuilt in full).
#
# Needs clang-cl, lld-link and the llvm-* tools, cmake, ninja, python3, git and PowerShell
# (MTR's tools/apply_game_patches.ps1 runs as `powershell`). When clang-cl is not on PATH the
# script re-runs itself in the distrobox container named by $PGR4_DISTROBOX (default pgr4dev).
# Run: proton/deck-run.sh.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$here/.." && pwd)
stages=all
root=$HOME/recomp/builds/pgr4
dest=""
game=$HOME/recomp/dumps/pgr4/game
jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
march=x86-64-v3
mtune=znver2
force=0
xwin=${XWIN_DIR:-$HOME/recomp/xwin}
while [[ $# -gt 0 ]]; do
  case $1 in
    --stage) stages=$2; shift 2 ;;
    --root) root=$2; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --game) game=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --march) march=$2; shift 2 ;;
    --mtune) mtune=$2; shift 2 ;;
    --force) force=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

if ! command -v clang-cl >/dev/null; then
  box=${PGR4_DISTROBOX:-pgr4dev}
  command -v distrobox >/dev/null || { echo "clang-cl not on PATH and no distrobox" >&2; exit 1; }
  exec distrobox enter "$box" -- "$here/build.sh" --stage "$stages" --root "$root" \
    ${dest:+--dest "$dest"} --game "$game" --jobs "$jobs" --march "$march" --mtune "$mtune" \
    $([[ $force == 1 ]] && echo --force)
fi

[[ -n $dest ]] || dest=$root/native-deck
src=$root/MTR-PGR4
tree=$dest/MTR-PGR4
sdk_src=$tree/thirdparty/rexglue-sdk
sdk_build=$sdk_src/out/build/native-release
sdk=$dest/sdk-install
host_build=$dest/host-sdk
host_rexglue=$sdk_src/out/linux-amd64/rexglue
toolchain=$repo_root/build/clang-cl-xwin.cmake
patches=("$repo_root/instrument/rexglue-drawcount.patch" "$here/rexglue-native-primary.patch"
  "$here/rexglue-runtime-perf.patch")
sources=("$here/src/pgr4_d3d_trace.cpp" "$here/src/d3d_surface.inc")
native_src=$here/src/native
# build.ps1's flags (see build-mtr.ps1 for why clang-cl needs them spelled out), with the
# target CPU from --march / --mtune. Precise FP, not ReXGlue's strict: strict turns every FP
# operation into a constrained intrinsic, which keeps LLVM from narrowing the guest's
# double(float(a op b)) to single precision, vectorizing, hoisting or scheduling FP code, and
# MTR's own clang-cl builds never had it (clang-cl drops the GNU spelling). The guest's FPSCR
# writes still reach MXCSR; the compiler only assumes round-to-nearest when folding constants,
# and the game sets the rounding mode in 5 places. Host FP exceptions are masked
# (FPSCRPlatform::InitHostExceptions), so speculated FP operations cannot trap. No caller reads
# errno after a math function, so sqrt and friends compile to bare instructions.
clang_cl_flags="/clang:-march=$march /clang:-mtune=$mtune /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t-"
export PATH=$HOME/recomp/tools/bin:$PATH

IFS=, read -ra stage_list <<< "$stages"
for s in "${stage_list[@]}"; do
  case $s in all|prep|sdk|game|install) ;; *) echo "unknown stage: $s" >&2; exit 2 ;; esac
done
want() { local s; for s in "${stage_list[@]}"; do [[ $s == all || $s == "$1" ]] && return 0; done; return 1; }
step() {
  local name=$1 t0=$SECONDS; shift
  echo "=== $name"
  "$@"
  echo "=== $name done in $((SECONDS - t0)) s"
}
need() { local f; for f in "$@"; do [[ -e $f ]] || { echo "missing $f" >&2; exit 1; }; done; }
# Configures build tree $1 with the cmake arguments after it. CMake takes CFLAGS/CXXFLAGS only
# when it creates a cache, so a tree configured with other compile flags starts over. A tree
# configured with other arguments is reconfigured, and an unchanged one is left alone: the SDK's
# configure step stamps include/rex/version.h with the time, and a new header rebuilds all that
# includes it, the generated code too. `cmake --build` still reconfigures after a CMakeLists.txt
# change.
configure() {
  local dir=$1; shift
  local flags_stamp=$dir/pgr4-compile-flags args_stamp=$dir/pgr4-configure-args args="$*"
  if [[ -f $dir/CMakeCache.txt && $(cat "$flags_stamp" 2>/dev/null) != "$clang_cl_flags" ]]; then
    echo "compile flags changed: configuring $dir from scratch"
    rm -rf "$dir/CMakeCache.txt" "$dir/CMakeFiles"
  fi
  [[ -f $dir/CMakeCache.txt && $(cat "$args_stamp" 2>/dev/null) == "$args" ]] && return 0
  CFLAGS=$clang_cl_flags CXXFLAGS=$clang_cl_flags cmake -B "$dir" "$@"
  printf '%s' "$clang_cl_flags" > "$flags_stamp"
  printf '%s' "$args" > "$args_stamp"
}

# The CMake block appended to the game's CMakeLists.txt; the same as build.ps1's, plus the host
# codegen tool: the rexglue.exe the cross SDK installs does not run here.
read -r -d '' native_cmake <<'CMAKE' || true
# begin pgr4-native native renderer (native/build.sh)
file(GLOB PGR4_NATIVE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")
target_sources(pgr4_recompiled PRIVATE ${PGR4_NATIVE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/src/graphics/pipeline/texture/util.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk/src/graphics/pipeline/texture/info_formats.cpp")
target_link_libraries(pgr4_recompiled PRIVATE d3d12 dxgi dxguid)
# pgr4_recompiled.map: function addresses for Linux perf samples of the PE image.
target_link_options(pgr4_recompiled PRIVATE /MAP)
# Shaders module: ReXGlue's DXBC translator sources as a static library.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/shader_xlate.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/src/native/shader_xlate.cmake")
    target_link_libraries(pgr4_recompiled PRIVATE pgr4_shader_xlate)
endif()
if(PGR4_HOST_REXGLUE)
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${PGR4_HOST_REXGLUE}")
endif()
# end pgr4-native native renderer
CMAKE

prep() {
  need "$src/thirdparty/rexglue-sdk/.git" "${patches[@]}" "${sources[@]}" "$native_src"
  mkdir -p "$dest"
  local keep=()
  # An already patched CMakeLists.txt stays, so a re-run of prep does not force a full rebuild.
  grep -q 'pgr4_d3d_trace\.cpp' "$tree/CMakeLists.txt" 2>/dev/null && keep=(--exclude=/CMakeLists.txt)
  rsync -a --exclude=/out --exclude=/sdk-install --exclude=/generated/default \
    --exclude=/geometry-wars/generated --exclude=/thirdparty/rexglue-sdk/out "${keep[@]}" "$src/" "$tree/"
  local p
  for p in "${patches[@]}"; do
    if git -C "$sdk_src" apply --check --reverse "$p" 2>/dev/null; then
      echo "$(basename "$p") already applied"
    else
      # Files a patch creates are not in the source, so the rsync above leaves an earlier prep's.
      git -C "$sdk_src" apply --summary "$p" | sed -n 's/^ create mode [0-7]* //p' |
        while read -r f; do rm -f "$sdk_src/$f"; done
      git -C "$sdk_src" apply "$p"
      echo "applied $(basename "$p")"
    fi
  done
  cp -f "${sources[@]}" "$tree/src/"
  rsync -a --delete "$native_src/" "$tree/src/native/"
  NATIVE_CMAKE=$native_cmake python3 - "$tree/CMakeLists.txt" <<'PY'
import os, re, sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
new = text
if "pgr4_d3d_trace.cpp" not in new:
    new, n = re.subn(r"(?m)^(\s*)src/pgr4_patches\.cpp$", r"\1src/pgr4_patches.cpp\n\1src/pgr4_d3d_trace.cpp", new)
    if n != 1:
        sys.exit("CMakeLists.txt: src/pgr4_patches.cpp line not found")
    print("added src/pgr4_d3d_trace.cpp to CMakeLists.txt")
want = os.environ["NATIVE_CMAKE"].strip() + "\n"
block = re.compile(r"(?s)# begin pgr4-native native renderer.*?# end pgr4-native native renderer\n?")
m = block.search(new)
if not m:
    new = new.rstrip() + "\n\n" + want
    print("added the src/native block to CMakeLists.txt")
elif m.group(0) != want:
    new = new[:m.start()] + want + new[m.end():]
    print("updated the src/native block in CMakeLists.txt")
if new != text:
    open(path, "w", encoding="utf-8", newline="\n").write(new)
PY
}

# Windows sources spell SDK header and library names in any case; the splat needs those spellings.
case_links() { python3 "$repo_root/build/xwin-case-links.py" "$xwin" "$tree" "$here/src"; }

sdk_stage() {
  need "$toolchain" "$xwin/crt/include"
  case_links
  # The codegen tool for this machine. Tracy stays off: nothing profiles the code generator.
  if [[ ! -x $host_rexglue || $force == 1 ]]; then
    cmake -S "$sdk_src" -B "$host_build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
      -DREXGLUE_ENABLE_TRACY=OFF
    cmake --build "$host_build" --target rexglue -j "$jobs"
  fi
  "$host_rexglue" --version
  # /MAP: rexruntime.map and rexgpu-xenos.map, function addresses for Linux perf samples.
  configure "$sdk_build" -S "$sdk_src" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP" \
    "-DCMAKE_INSTALL_PREFIX=$sdk"
  cmake --build "$sdk_build" --target install -j "$jobs"
}

game_stage() {
  need "$game/default.xex" "$host_rexglue" "$sdk/lib/cmake/rexglue/rexglueConfig.cmake"
  local manifest=$tree/pgr4_local.toml
  # Same manifest rewrite as build.ps1: the game path, and register-as-local codegen (see the
  # comment there for why these five options and not the other three).
  python3 - "$tree/pgr4_recompiled_manifest.toml" "$manifest" "$game" <<'PY'
import re, sys
src, dst, game = sys.argv[1:]
text = open(src, encoding="utf-8").read()
text = re.sub(r'game_root = "[^"]*"', f'game_root = "{game}"', text)
text = re.sub(r'file_path = "[^"]*"', f'file_path = "{game}/default.xex"', text)
if re.search(r"(?m)^\s*((cr|ctr|xer|non_volatile|non_argument|reserved)_as_local|skip_lr|skip_msr)\s*=", text):
    sys.exit("the MTR manifest already sets codegen register options; merge by hand")
m = re.search(r"(?m)^\[entrypoint\]\n", text)
if not m:
    sys.exit("pgr4_recompiled_manifest.toml: [entrypoint] not found")
locals_ = "".join(f"{k}_as_local = true\n" for k in ("cr", "ctr", "xer", "non_volatile", "reserved"))
new = text[:m.end()] + locals_ + text[m.end():]
# Unchanged content keeps its timestamp: the codegen step depends on this file (codegen.d),
# and a rewrite would regenerate and recompile all generated code.
try:
    same = open(dst, encoding="utf-8").read() == new
except FileNotFoundError:
    same = False
if not same:
    open(dst, "w", encoding="utf-8", newline="\n").write(new)
PY
  local stamp=$tree/generated/default/codegen.build.stamp
  [[ $force == 1 ]] && rm -f "$stamp"
  # Codegen rewrites its stamp even when its output does not change, and every generated object
  # depends on the stamp (through MTR's patch step), so a run with the inputs in codegen.d
  # unchanged would recompile all generated code.
  if [[ ! -f $stamp || $manifest -nt $stamp || $game/default.xex -nt $stamp || $host_rexglue -nt $stamp ]]; then
    (cd "$tree" && "$host_rexglue" codegen "$manifest")
  fi
  # As in build.ps1: MTR's patch script matches ctx.r31-style text that the options above turn
  # into locals. prep restores MTR's original each time; the rewrite keeps the original's
  # timestamp, so Ninja reruns the patch script (and recompiles all generated code) only when
  # MTR's script changes.
  python3 - "$tree/tools/apply_game_patches.ps1" <<'PY'
import os, re, sys
path = sys.argv[1]
st = os.stat(path)
text = open(path, encoding="utf-8").read()
new = re.sub(r"\bctx\.(r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|cr[0-7]|ctr|xer)\b", r"\1", text)
if new != text:
    open(path, "w", encoding="utf-8", newline="\n").write(new)
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
PY
  case_links
  configure "$tree/out/pgr4" -S "$tree" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" \
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$sdk" "-DMTR_GAME_MANIFEST=$manifest" \
    "-DPGR4_HOST_REXGLUE=$host_rexglue"
  cmake --build "$tree/out/pgr4" -j "$jobs"
  ls -l "$tree/out/pgr4/pgr4_recompiled.exe"
}

install_stage() {
  local runtime=$dest/run/runtime
  local files=("$tree/out/pgr4/pgr4_recompiled.exe" "$sdk/bin/rexruntime.dll" "$sdk/bin/rexgpu-xenos.dll")
  need "${files[@]}"
  mkdir -p "$runtime"
  cp -f "${files[@]}" "$runtime/"
  # Their linker maps, for deck-run.sh --perf (perf_symbols.py).
  local map
  for map in "$tree/out/pgr4/pgr4_recompiled.map" "$sdk_src"/out/win-amd64/{rexruntime,rexgpu-xenos}.map; do
    [[ -f $map ]] && cp -f "$map" "$runtime/"
  done
  ls -l "$runtime"
}

want prep && step prep prep
want sdk && step sdk sdk_stage
want game && step game game_stage
want install && step install install_stage
exit 0
