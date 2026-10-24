#!/usr/bin/env bash
# Builds the PGR4 native-renderer tree on Linux (written on a Steam Deck), the counterpart of
# build.ps1: the same stages, sources, patches and compiler flags, cross-compiled for Windows x64
# with clang-cl and lld-link against an xwin splat of the MSVC CRT and Windows SDK. The result
# runs under Proton like a build from the Windows host. The MTR build under $root/MTR-PGR4 is
# only read, never modified.
#
#   native/build.sh [--stage prep,sdk,game,install] [--root DIR] [--dest DIR] [--game DIR]
#                   [--jobs N] [--march x86-64-v3] [--mtune znver2] [--pgo gen|off|FILE] [--force]
#
# Stages, in order (default: all):
#   prep     rsync $root/MTR-PGR4 (source and patched SDK source; no build dirs, no generated
#            code) to $dest/MTR-PGR4, apply rexglue-drawcount.patch, rexglue-native-primary.patch,
#            rexglue-runtime-perf.patch and rexglue-content.patch to the SDK copy, copy
#            src/pgr4_d3d_trace.cpp and src/d3d_surface.inc into the game's src/, mirror src/native/
#            and add both to the game's CMakeLists.txt; rewrite the base-executable addresses in the
#            copy's game patches (tools/apply_game_patches.ps1, src/pgr4_patches.cpp) with Title
#            Update #2's (title-update/addresses.txt), and apply title-update/apply_game_patches.patch
#            (the one patched function TU#2 changed, and the presentation interval the native
#            renderer's frame pacer sets).
#   sdk      build the codegen tool (rexglue) for this machine into $dest/host-sdk, then
#            cross-build the SDK copy and install it into $dest/sdk-install.
#   game     codegen Title Update #2: $game/default.xex with title-update/default.xexp beside it
#            (copies in $dest/title-update-image), with the copy's manifest and TU#2's entry
#            options (title-update/entrypoint.toml); MTR's game patches included. Build
#            out/pgr4/pgr4_recompiled.exe.
#   install  $dest/run/runtime: exe, rexruntime.dll, rexgpu-xenos.dll, and title-update/ (the
#            runtime's update:\ when no --update_data_root is given).
#
# --march / --mtune set the target CPU of the SDK and the game. The default is the Steam Deck's
# Zen 2: x86-64-v3 brings FMA (the recompiled code's thousands of std::fma calls become one
# instruction instead of a call into the CRT's software fma, Wine's under Proton), AVX2 for the
# SIMD emulation and MOVBE for the byte-swapped guest loads. --march x86-64-v2 builds for the
# older CPUs build.ps1 targets. A tree configured with other flags is reconfigured (and so
# rebuilt in full).
#
# --pgo FILE builds the game (not the SDK) optimized with the profile FILE, copied to $dest/pgo
# under its hash (a new profile is new flags); without --pgo, $dest/pgo/default.profdata is used
# when it exists, and --pgo off builds without one. --pgo gen builds the game instrumented: a
# run of it writes run/runtime/pgr4_recompiled.profraw (src/native/pgo_counters.cpp), and
# `llvm-profdata merge -o $dest/pgo/default.profdata <profraw>` makes it the default. The profile
# runtime the instrumented build links, clang_rt.profile.lib, is compiled here from the
# compiler-rt source of the clang-cl in use: the Linux LLVM packages have no Windows one. A
# profile goes stale only where the code changes; clang warns per file and compiles the changed
# functions without it. Like --march, a change of profile rebuilds the game in full.
#
# --force rebuilds the codegen tool (sdk) and regenerates all game code (game).
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
pgo=auto
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
    --pgo) pgo=$2; shift 2 ;;
    --force) force=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

case $pgo in
  auto|off|gen) ;;
  *) [[ -f $pgo ]] || { echo "--pgo: no profile $pgo" >&2; exit 2; }; pgo=$(realpath "$pgo") ;;
esac

if ! command -v clang-cl >/dev/null; then
  box=${PGR4_DISTROBOX:-pgr4dev}
  command -v distrobox >/dev/null || { echo "clang-cl not on PATH and no distrobox" >&2; exit 1; }
  exec distrobox enter "$box" -- "$here/build.sh" --stage "$stages" --root "$root" \
    ${dest:+--dest "$dest"} --game "$game" --jobs "$jobs" --march "$march" --mtune "$mtune" \
    --pgo "$pgo" $([[ $force == 1 ]] && echo --force)
fi

[[ -n $dest ]] || dest=$root/native-deck
if [[ $pgo == auto ]]; then
  pgo=off
  [[ -f $dest/pgo/default.profdata ]] && pgo=$dest/pgo/default.profdata
fi
src=$root/MTR-PGR4
tree=$dest/MTR-PGR4
sdk_src=$tree/thirdparty/rexglue-sdk
sdk_build=$sdk_src/out/build/native-release
sdk=$dest/sdk-install
host_build=$dest/host-sdk
host_rexglue=$sdk_src/out/linux-amd64/rexglue
toolchain=$repo_root/build/clang-cl-xwin.cmake
patches=("$repo_root/instrument/rexglue-drawcount.patch" "$here/rexglue-native-primary.patch"
  "$here/rexglue-runtime-perf.patch" "$here/rexglue-content.patch")
title_update=$repo_root/title-update
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
#
# 128-bit vectors where the compiler picks the width: a function that writes a 256-bit register
# runs VZEROUPPER before every call and return, and on the Deck, whose microcode has no Zenbleed
# fix (the kernel sets the chicken bit instead), the instruction after it took 4 % of the game
# and render threads' samples (td-145). -mprefer-vector-width covers the vectorizers; struct
# copies and fills follow allow-light-256-bit, a tuning feature of the Zen CPUs that lets them
# use 256 bits regardless. Intrinsics keep their width.
clang_cl_flags="/clang:-march=$march /clang:-mtune=$mtune /clang:-mprefer-vector-width=128 -Xclang -target-feature -Xclang -allow-light-256-bit /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t-"
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
# Configures build tree $1 with compile flags $2, linker flags $3 and the cmake arguments after
# them. CMake takes CFLAGS/CXXFLAGS/LDFLAGS only when it creates a cache, so a tree configured
# with other flags starts over (and Ninja, seeing new command lines, rebuilds every object). A
# tree configured with other arguments is reconfigured, and an unchanged one is left alone: the
# SDK's configure step stamps include/rex/version.h with the time, and a new header rebuilds all
# that includes it, the generated code too. `cmake --build` still reconfigures after a
# CMakeLists.txt change.
configure() {
  local dir=$1 flags=$2 ldflags=$3; shift 3
  local flags_stamp=$dir/pgr4-compile-flags args_stamp=$dir/pgr4-configure-args args="$*"
  if [[ -f $dir/CMakeCache.txt && $(cat "$flags_stamp" 2>/dev/null) != "$flags$ldflags" ]]; then
    echo "compile flags changed: configuring $dir from scratch"
    rm -rf "$dir/CMakeCache.txt" "$dir/CMakeFiles"
  fi
  [[ -f $dir/CMakeCache.txt && $(cat "$args_stamp" 2>/dev/null) == "$args" ]] && return 0
  CFLAGS=$flags CXXFLAGS=$flags LDFLAGS=$ldflags cmake -B "$dir" "$@"
  printf '%s' "$flags$ldflags" > "$flags_stamp"
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
  need "$src/thirdparty/rexglue-sdk/.git" "${patches[@]}" "${sources[@]}" "$native_src" \
    "$title_update/addresses.txt" "$title_update/apply_game_patches.patch"
  mkdir -p "$dest"
  local keep=()
  # An already patched CMakeLists.txt stays, so a re-run of prep does not force a full rebuild.
  grep -q 'pgr4_d3d_trace\.cpp' "$tree/CMakeLists.txt" 2>/dev/null && keep=(--exclude=/CMakeLists.txt)
  rsync -a --exclude=/out --exclude=/sdk-install --exclude=/generated/default \
    --exclude=/geometry-wars/generated --exclude=/thirdparty/rexglue-sdk/out \
    --exclude=/src/pgr4_patches.cpp "${keep[@]}" "$src/" "$tree/"
  # MTR's game patches name the base executable's addresses; the code is Title Update #2's, which
  # also changed one patched function; title-update/apply_game_patches.patch adapts that one and
  # hands the presentation interval to the native renderer (Pgr4PresentationInterval). The patch
  # script takes the time of the newest of its inputs: Ninja reruns it (and recompiles all generated
  # code) after every codegen anyway, and otherwise only when one of them changes.
  # pgr4_patches.cpp is replaced only when its text changes, so it recompiles only then.
  local retarget=("$here/tools/tu_address_map.py" retarget "$title_update/addresses.txt")
  local patcher=$tree/tools/apply_game_patches.ps1 newest=$src/tools/apply_game_patches.ps1 f
  cp "$newest" "$patcher"
  python3 "${retarget[@]}" "$patcher"
  git -C "$tree" apply --ignore-whitespace "$title_update/apply_game_patches.patch"
  for f in "$title_update/addresses.txt" "$title_update/apply_game_patches.patch"; do
    [[ $f -nt $newest ]] && newest=$f
  done
  touch -r "$newest" "$patcher"
  local tmp
  tmp=$(mktemp)
  cp "$src/src/pgr4_patches.cpp" "$tmp"
  python3 "${retarget[@]}" "$tmp"
  cmp -s "$tmp" "$tree/src/pgr4_patches.cpp" || cp "$tmp" "$tree/src/pgr4_patches.cpp"
  rm -f "$tmp"
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

# clang_rt.profile.lib for --pgo gen: the compiler-rt profile runtime of this clang-cl, the
# sources compiler-rt's CMakeLists.txt lists for Windows, built with the game's dynamic CRT (/MD,
# no default libraries named); compiler-rt's own build would bring the static CRT along.
# Sets profile_rt_dir.
profile_rt() {
  local ver src obj f d inc=()
  ver=$(clang-cl --version | sed -n '1s/.*clang version \([0-9][0-9.]*\).*/\1/p')
  profile_rt_dir=$dest/profile-rt/$ver
  [[ -f $profile_rt_dir/clang_rt.profile.lib ]] && return 0
  src=$dest/profile-rt/llvm-$ver
  if [[ ! -d $src/compiler-rt/lib/profile ]]; then
    git clone -q --depth 1 --filter=blob:none --sparse --branch "llvmorg-$ver" \
      https://github.com/llvm/llvm-project.git "$src"
    git -C "$src" sparse-checkout set --no-cone /compiler-rt/lib/profile/ /compiler-rt/include/profile/
  fi
  obj=$profile_rt_dir/obj
  mkdir -p "$obj"
  for d in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared; do inc+=("/imsvc$xwin/$d"); done
  for f in "$src"/compiler-rt/lib/profile/*.c "$src/compiler-rt/lib/profile/InstrProfilingRuntime.cpp"; do
    clang-cl --target=x86_64-pc-windows-msvc /nologo /c /O2 /MD /Zl /GS- /W0 -DCOMPILER_RT_HAS_ATOMICS=1 \
      "/I$src/compiler-rt/lib" "/I$src/compiler-rt/include" "${inc[@]}" "$f" "/Fo$obj/$(basename "${f%.*}").obj"
  done
  llvm-lib /nologo "/out:$profile_rt_dir/clang_rt.profile.lib" "$obj"/*.obj
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
  configure "$sdk_build" "$clang_cl_flags" "" -S "$sdk_src" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP" \
    "-DCMAKE_INSTALL_PREFIX=$sdk"
  cmake --build "$sdk_build" --target install -j "$jobs"
}

game_stage() {
  need "$game/default.xex" "$host_rexglue" "$sdk/lib/cmake/rexglue/rexglueConfig.cmake" \
    "$title_update/default.xexp" "$title_update/entrypoint.toml" "$title_update/addresses.txt"
  local manifest=$tree/pgr4_local.toml image=$dest/title-update-image f
  # Codegen applies an XEX patch found beside the XEX it loads: a copy of the disc's default.xex
  # with TU#2's default.xexp next to it (copied only when changed, the codegen check below
  # compares their times).
  mkdir -p "$image"
  for f in "$game/default.xex" "$title_update/default.xexp"; do
    cmp -s "$f" "$image/$(basename "$f")" || cp -p "$f" "$image/"
  done
  # Same manifest rewrite as build.ps1: the image path, register-as-local codegen (see the
  # comment there for why these five options and not the other three), and TU#2's setjmp,
  # longjmp and function entries in place of MTR's, each of which addresses.txt must account for.
  python3 - "$tree/pgr4_recompiled_manifest.toml" "$manifest" "$image" "$title_update" <<'PY'
import re, sys
src, dst, image, tu = sys.argv[1:]
text = open(src, encoding="utf-8").read()
text = re.sub(r'game_root = "[^"]*"', f'game_root = "{image}"', text)
text = re.sub(r'file_path = "[^"]*"', f'file_path = "{image}/default.xex"', text)
if re.search(r"(?m)^\s*((cr|ctr|xer|non_volatile|non_argument|reserved)_as_local|skip_lr|skip_msr)\s*=", text):
    sys.exit("the MTR manifest already sets codegen register options; merge by hand")
m = re.search(r"(?m)^\[entrypoint\]\n", text)
if not m:
    sys.exit("pgr4_recompiled_manifest.toml: [entrypoint] not found")
known = {int(line.split()[0], 16) for line in open(f"{tu}/addresses.txt", encoding="utf-8")
         if re.match(r"82[0-9A-Fa-f]{6}\s", line)}
funcs = re.search(r"(?ms)^\[entrypoint\.functions\]\n(.*?)(?=^\[|\Z)", text)
if not funcs:
    sys.exit("pgr4_recompiled_manifest.toml: [entrypoint.functions] not found")
mtr = {int(a, 16) for a in re.findall(r"(?m)^\s*0x([0-9A-Fa-f]{8})\s*=", funcs.group(1))}
mtr |= {int(a, 16) for a in re.findall(r"(?m)^\s*(?:setjmp|longjmp)_address\s*=\s*0x([0-9A-Fa-f]{8})", text)}
if unknown := sorted(mtr - known):
    sys.exit("MTR's manifest has entries title-update/addresses.txt lacks: "
             + ", ".join(f"0x{a:08X}" for a in unknown)
             + " (native/tools/tu_address_map.py maps them)")
entry = open(f"{tu}/entrypoint.toml", encoding="utf-8").read()
jumps = "".join(re.findall(r"(?m)^(?:setjmp|longjmp)_address\s*=.*\n", entry))
text = text[:funcs.start()] + entry[entry.index("[entrypoint.functions]"):] + text[funcs.end():]
text = re.sub(r"(?m)^(?:setjmp|longjmp)_address\s*=.*\n", "", text)
m = re.search(r"(?m)^\[entrypoint\]\n", text)
locals_ = "".join(f"{k}_as_local = true\n" for k in ("cr", "ctr", "xer", "non_volatile", "reserved"))
new = text[:m.end()] + locals_ + jumps + text[m.end():]
# Unchanged content keeps its timestamp: the codegen step depends on this file (codegen.d),
# and a rewrite would regenerate and recompile all generated code.
try:
    same = open(dst, encoding="utf-8").read() == new
except FileNotFoundError:
    same = False
if not same:
    open(dst, "w", encoding="utf-8", newline="\n").write(new)
PY
  local stamp=$tree/generated/default/codegen.build.stamp ignore_stamp=()
  # Codegen skips a module whose inputs it fingerprinted before, and the fingerprint leaves out
  # the codegen tool itself: a rebuilt tool has to regenerate past it.
  [[ $force == 1 || ($host_rexglue -nt $stamp && -f $stamp) ]] && ignore_stamp=(--ignore-stamp)
  [[ $force == 1 ]] && rm -f "$stamp"
  # Codegen rewrites its stamp even when its output does not change, and every generated object
  # depends on the stamp (through MTR's patch step), so a run with the inputs in codegen.d
  # unchanged would recompile all generated code.
  if [[ ! -f $stamp || $manifest -nt $stamp || $image/default.xex -nt $stamp ||
        $image/default.xexp -nt $stamp || $host_rexglue -nt $stamp ]]; then
    (cd "$tree" && "$host_rexglue" codegen "${ignore_stamp[@]}" "$manifest")
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
# Not \b in front: PowerShell's `t and `n run into the name (`tctx.r29).
new = re.sub(r"(?:(?<!\w)|(?<=`[tn]))ctx\.(r1[4-9]|r2[0-9]|r3[01]|f1[4-9]|f2[0-9]|f3[01]|cr[0-7]|ctr|xer)\b", r"\1", text)
if new != text:
    open(path, "w", encoding="utf-8", newline="\n").write(new)
    os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
PY
  case_links
  # The MSVC CRT deprecates fopen, fscanf and friends in favor of its _s variants; the native
  # code uses the standard functions.
  local flags="$clang_cl_flags /D_CRT_SECURE_NO_WARNINGS" ldflags="" sum
  case $pgo in
    off) ;;
    gen)
      profile_rt
      flags+=" /clang:-fprofile-generate"
      ldflags="/libpath:$profile_rt_dir" ;;
    *)
      sum=$(sha1sum < "$pgo" | cut -c1-12)
      mkdir -p "$dest/pgo"
      cmp -s "$pgo" "$dest/pgo/$sum.profdata" || cp "$pgo" "$dest/pgo/$sum.profdata"
      flags+=" /clang:-fprofile-use=$dest/pgo/$sum.profdata" ;;
  esac
  configure "$tree/out/pgr4" "$flags" "$ldflags" -S "$tree" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_PREFIX_PATH=$sdk" "-DMTR_GAME_MANIFEST=$manifest" "-DPGR4_HOST_REXGLUE=$host_rexglue"
  cmake --build "$tree/out/pgr4" -j "$jobs"
  ls -l "$tree/out/pgr4/pgr4_recompiled.exe"
}

install_stage() {
  local runtime=$dest/run/runtime
  local files=("$tree/out/pgr4/pgr4_recompiled.exe" "$sdk/bin/rexruntime.dll" "$sdk/bin/rexgpu-xenos.dll")
  need "${files[@]}"
  mkdir -p "$runtime/title-update"
  # New files, not rewritten ones: a running game keeps the image it mapped, and a perf run's
  # hard links (deck-run.sh --perf) keep the modules it sampled.
  cp --remove-destination "${files[@]}" "$runtime/"
  cp -f "$title_update/default.xexp" "$title_update/Pgr4TitleUpdate.bin" "$runtime/title-update/"
  # Their linker maps, for deck-run.sh --perf (perf_symbols.py).
  local map
  for map in "$tree/out/pgr4/pgr4_recompiled.map" "$sdk_src"/out/win-amd64/{rexruntime,rexgpu-xenos}.map; do
    [[ -f $map ]] && cp --remove-destination "$map" "$runtime/"
  done
  ls -l "$runtime"
}

want prep && step prep prep
want sdk && step sdk sdk_stage
want game && step game game_stage
want install && step install install_stage
exit 0
