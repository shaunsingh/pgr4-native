#!/usr/bin/env bash
# Builds Project Gotham Racing 4 (Title Update #2) with the native renderer on Linux (written on a
# Steam Deck), cross-compiled for Windows x64 with clang-cl and lld-link against an xwin splat of
# the MSVC CRT and Windows SDK. The result runs under Proton, or on Windows.
#
#   scripts/build.sh [--stage sdk,game,install] [--dest DIR] [--game DIR] [--update PATH]
#                   [--jobs N] [--march x86-64-v3] [--mtune znver2] [--pgo gen|off|FILE] [--force]
#
# Stages, in order (default: all):
#   sdk      $dest/rexglue-sdk: ReXGlue at the commit patches/rexglue.lock names, with the patches in
#            patches/ applied in order (cloned on first use; reset and re-patched when the lock
#            or a patch changes). Build its codegen tool for this machine into $dest/host-sdk, then
#            cross-build the SDK and install it into $dest/sdk-install.
#   game     $dest/game: the game projects (games/), each with its game data in game-data/.
#            games/pgr4: $game/default.xex with Title Update #2's $update/default.xexp beside it,
#            where codegen applies the update; out/pgr4_recompiled.exe with the renderer
#            (src/). games/gw: $game/gw.xex, the garage's Geometry Wars cabinet;
#            out/gw_recompiled.exe.
#            $dest/fidelityfx-sdk: the AMD FidelityFX SDK at the commit scripts/fidelityfx.lock
#            names, only its upscaler API headers and signed DLLs.
#   install  $dest/run/runtime: both executables, rexruntime.dll, rexgpu-xenos.dll, AMD's FidelityFX
#            loader and upscaler DLLs (--pgr4_native_upscaler), and title-update/ (the runtime's
#            update:\ when no --update_data_root is given).
#
# --update PATH is the player's copy of Title Update #2: its package as downloaded
# (TU_16L61VP_0000004000000.00000000000G1), extracted into $dest/title-update with the SDK's
# `rexglue vfs-dump`, or a folder that holds its two files, default.xexp and Pgr4TitleUpdate.bin.
# games/pgr4/title-update.sha256 identifies the two: the generated code matches only that update.
#
# --march / --mtune set the target CPU of the SDK and the game. The default is the Steam Deck's
# Zen 2: x86-64-v3 brings FMA (the recompiled code's thousands of std::fma calls become one
# instruction instead of a call into the CRT's software fma, Wine's under Proton), AVX2 for the
# SIMD emulation and MOVBE for the byte-swapped guest loads. --march x86-64-v2 builds for older
# CPUs. A tree configured with other flags is reconfigured (and so rebuilt in full).
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
# Needs clang-cl, lld-link and the llvm-* tools, cmake, ninja, python3 and git. When clang-cl is
# not on PATH the script re-runs itself in the distrobox container named by $PGR4_DISTROBOX
# (default pgr4dev). Run: scripts/deck-run.sh.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$here/.." && pwd)
stages=all
dest=$HOME/recomp/builds/pgr4/native-deck
game=$HOME/recomp/dumps/pgr4/game
update=$HOME/recomp/dumps/pgr4/title-update
jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))
march=x86-64-v3
mtune=znver2
force=0
pgo=auto
xwin=${XWIN_DIR:-$HOME/recomp/xwin}
while [[ $# -gt 0 ]]; do
  case $1 in
    --stage) stages=$2; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --game) game=$2; shift 2 ;;
    --update) update=$2; shift 2 ;;
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
  exec distrobox enter "$box" -- "$here/build.sh" --stage "$stages" --dest "$dest" \
    --game "$game" --update "$update" --jobs "$jobs" --march "$march" --mtune "$mtune" \
    --pgo "$pgo" $([[ $force == 1 ]] && echo --force)
fi

if [[ $pgo == auto ]]; then
  pgo=off
  [[ -f $dest/pgo/default.profdata ]] && pgo=$dest/pgo/default.profdata
fi
# shellcheck source=../patches/rexglue.lock
source "$repo_root/patches/rexglue.lock"
# shellcheck source=fidelityfx.lock
source "$here/fidelityfx.lock"
sdk_patches=("$repo_root"/patches/*.patch)
sdk_src=$dest/rexglue-sdk
sdk_build=$sdk_src/out/build/native-release
sdk=$dest/sdk-install
host_build=$dest/host-sdk
host_rexglue=$sdk_src/out/linux-amd64/rexglue
ffx_src=$dest/fidelityfx-sdk
ffx_kit=$ffx_src/Kits/FidelityFX
ffx_dlls=("$ffx_kit/signedbin/amd_fidelityfx_loader_dx12.dll" "$ffx_kit/signedbin/amd_fidelityfx_upscaler_dx12.dll")
titles=(gw pgr4)
toolchain=$repo_root/cmake/clang-cl-xwin.cmake
title_update_sums=$repo_root/games/pgr4/title-update.sha256
# Precise FP, not ReXGlue's strict: strict turns every FP operation into a constrained intrinsic,
# which keeps LLVM from narrowing the guest's double(float(a op b)) to single precision,
# vectorizing, hoisting or scheduling FP code (clang-cl drops the GNU spelling of the SDK's
# flag). The guest's FPSCR writes still reach MXCSR; the compiler only assumes round-to-nearest
# when folding constants, and the game sets the rounding mode in 5 places. Host FP exceptions are
# masked (FPSCRPlatform::InitHostExceptions), so speculated FP operations cannot trap. No caller
# reads errno after a math function, so sqrt and friends compile to bare instructions.
#
# 128-bit vectors where the compiler picks the width: a function that writes a 256-bit register
# runs VZEROUPPER before every call and return, and on the Deck, whose microcode has no Zenbleed
# fix (the kernel sets the chicken bit instead), the instruction after it took 4 % of the game
# and render threads' samples (td-145). -mprefer-vector-width covers the vectorizers; struct
# copies and fills follow allow-light-256-bit, a tuning feature of the Zen CPUs that lets them
# use 256 bits regardless. Intrinsics keep their width.
#
# -mprfchw: PREFETCHW, so that __builtin_prefetch(p, 1) prefetches for a write; x86-64-v3 leaves
# it out, and clang then emits a read prefetch. Zen 2 has it; Intel CPUs before Broadwell, which
# do not report it, run it as a NOP. These flags reach only the toolchain's AMD64 targets (the SDK
# and the game); the host codegen tool and the profile runtime are compiled with their own.
clang_cl_flags="/clang:-march=$march /clang:-mtune=$mtune /clang:-mprefer-vector-width=128 /clang:-mprfchw -Xclang -target-feature -Xclang -allow-light-256-bit /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t-"
export PATH=$HOME/recomp/tools/bin:$PATH

IFS=, read -ra stage_list <<< "$stages"
for s in "${stage_list[@]}"; do
  case $s in all|sdk|game|install) ;; *) echo "unknown stage: $s" >&2; exit 2 ;; esac
done
want() { local s; for s in "${stage_list[@]}"; do [[ $s == all || $s == "$1" ]] && return 0; done; return 1; }
step() {
  local name=$1 t0=$SECONDS; shift
  echo "=== $name"
  "$@"
  echo "=== $name done in $((SECONDS - t0)) s"
}
need() { local f; for f in "$@"; do [[ -e $f ]] || { echo "missing $f" >&2; exit 1; }; done; }
# The player's Title Update #2 files in $update, checked against the ones the code is generated for.
# A package is extracted first, every time: it takes no time, and its files reach the game data
# and the install only when they differ.
need_update() {
  if [[ -f $update ]]; then
    need "$host_rexglue"
    rm -rf "$dest/title-update"
    "$host_rexglue" vfs-dump "$update" "$dest/title-update"
    update=$dest/title-update
  fi
  (cd "$update" && sha256sum --quiet -c "$title_update_sums") ||
    { echo "$update: not Title Update #2's files (README.md)" >&2; exit 1; }
}
# Configures build tree $1 with compile flags $2, linker flags $3 and the cmake arguments after
# them. CMake takes CFLAGS/CXXFLAGS/LDFLAGS and the toolchain file only when it creates a cache,
# so a tree configured with other flags or another toolchain file starts over (and Ninja, seeing
# new command lines, rebuilds every object). A
# tree configured with other arguments is reconfigured, and an unchanged one is left alone: the
# SDK's configure step stamps include/rex/version.h with the time, and a new header rebuilds all
# that includes it, the generated code too. `cmake --build` still reconfigures after a
# CMakeLists.txt change.
configure() {
  local dir=$1 flags=$2 ldflags=$3; shift 3
  local flags_stamp=$dir/pgr4-compile-flags args_stamp=$dir/pgr4-configure-args args="$*"
  local cache_inputs="$flags$ldflags $toolchain"
  if [[ -f $dir/CMakeCache.txt && $(cat "$flags_stamp" 2>/dev/null) != "$cache_inputs" ]]; then
    echo "compile flags or toolchain changed: configuring $dir from scratch"
    rm -rf "$dir/CMakeCache.txt" "$dir/CMakeFiles"
  fi
  [[ -f $dir/CMakeCache.txt && $(cat "$args_stamp" 2>/dev/null) == "$args" ]] && return 0
  CFLAGS=$flags CXXFLAGS=$flags LDFLAGS=$ldflags cmake -B "$dir" "$@"
  printf '%s' "$cache_inputs" > "$flags_stamp"
  printf '%s' "$args" > "$args_stamp"
}

# $sdk_src at $REXGLUE_COMMIT with patches applied. The series' hash is kept in the checkout;
# an unchanged series leaves every file alone. A changed one resets the tree to the commit, which
# rewrites only the files the old series touched, and applies the new one, so a rebuild compiles
# only what the two series change.
sdk_checkout() {
  local series stamp=$sdk_src/.pgr4-series p
  need "${sdk_patches[@]}"
  series=$(cat "$repo_root/patches/rexglue.lock" "${sdk_patches[@]}" | sha256sum | cut -c1-16)
  if [[ ! -d $sdk_src/.git ]]; then
    git clone -q --no-checkout --filter=blob:none "$REXGLUE_URL" "$sdk_src"
  fi
  [[ $(cat "$stamp" 2>/dev/null) == "$series" ]] && { echo "SDK series $series already applied"; return 0; }
  git -C "$sdk_src" cat-file -e "$REXGLUE_COMMIT^{commit}" 2>/dev/null ||
    git -C "$sdk_src" fetch -q origin "$REXGLUE_COMMIT"
  rm -f "$stamp"
  git -C "$sdk_src" reset -q --hard "$REXGLUE_COMMIT"
  git -C "$sdk_src" clean -fdq -e /out
  git -C "$sdk_src" submodule sync -q --recursive
  git -C "$sdk_src" submodule update -q --init --recursive --depth 1 --jobs "$jobs"
  git -C "$sdk_src" submodule foreach -q --recursive 'git reset -q --hard && git clean -fdq'
  for p in "${sdk_patches[@]}"; do
    git -C "$sdk_src" apply --whitespace=nowarn "$p"
    echo "applied $(basename "$p")"
  done
  echo "$series" > "$stamp"
}

# $ffx_src: the FidelityFX SDK at $FFX_COMMIT, a shallow sparse checkout of what the build and the
# install use, without the SDK's samples, media and effect sources.
ffx_checkout() {
  [[ $(git -C "$ffx_src" rev-parse -q --verify HEAD 2>/dev/null) == "$FFX_COMMIT" ]] &&
    { echo "FidelityFX SDK $FFX_COMMIT already checked out"; return 0; }
  rm -rf "$ffx_src"
  git init -q "$ffx_src"
  git -C "$ffx_src" remote add origin "$FFX_URL"
  git -C "$ffx_src" sparse-checkout set --no-cone /Kits/FidelityFX/api/include/ \
    /Kits/FidelityFX/upscalers/include/ /Kits/FidelityFX/docs/license.md "${ffx_dlls[@]#"$ffx_src"}"
  git -C "$ffx_src" fetch -q --depth 1 --filter=blob:none origin "$FFX_COMMIT"
  git -C "$ffx_src" checkout -q --detach FETCH_HEAD
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
case_links() { python3 "$repo_root/scripts/xwin-case-links.py" "$xwin" "$@"; }

sdk_stage() {
  need "$toolchain" "$xwin/crt/include"
  step "sdk checkout" sdk_checkout
  case_links "$sdk_src"
  # The codegen tool for this machine. Tracy stays off: nothing profiles the code generator.
  if [[ ! -x $host_rexglue || $force == 1 || $sdk_src/.pgr4-series -nt $host_rexglue ]]; then
    cmake -S "$sdk_src" -B "$host_build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
      -DREXGLUE_ENABLE_TRACY=OFF
    cmake --build "$host_build" --target rexglue -j "$jobs"
  fi
  "$host_rexglue" --version
  # /MAP: rexruntime.map and rexgpu-xenos.map, function addresses for Linux perf samples. No
  # Tracy: nothing ships it (its client DLL is not part of the runtime).
  configure "$sdk_build" "$clang_cl_flags" "" -S "$sdk_src" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP" \
    -DREXGLUE_ENABLE_TRACY=OFF "-DCMAKE_INSTALL_PREFIX=$sdk"
  cmake --build "$sdk_build" --target install -j "$jobs"
}

# Builds title $1 (game/$1, the executable $1_recompiled.exe) from the game data files after it,
# staged in its game-data/, with compile flags $flags. The games' sources are staged in
# $dest/game beside their game data and generated code; -a keeps the sources' times, so an
# unchanged file does not recompile.
build_title() {
  local title=$1; shift
  local project=$dest/game/$title f
  local stamp=$project/generated/default/codegen.build.stamp ignore_stamp=()
  mkdir -p "$project/game-data"
  # Codegen applies an XEX patch found beside the XEX it loads (codegen.d lists both, so they are
  # copied only when changed).
  for f in "$@"; do
    cmp -s "$f" "$project/game-data/$(basename "$f")" || cp -p "$f" "$project/game-data/"
  done
  # The build reruns codegen when an input codegen.d lists changes. Codegen runs here first when
  # there is no generated code yet (the project's CMakeLists.txt includes generated/rexglue.cmake),
  # and when the codegen tool changed: its fingerprint of the inputs leaves out the tool itself.
  [[ $force == 1 || ($host_rexglue -nt $stamp && -f $stamp) ]] && ignore_stamp=(--ignore-stamp)
  if [[ ! -f $project/generated/rexglue.cmake || ! -f $stamp || ${#ignore_stamp[@]} -gt 0 ]]; then
    (cd "$project" && "$host_rexglue" codegen "${ignore_stamp[@]}" "${title}recompiled_manifest.toml")
  fi
  configure "$project/out" "$flags" "$ldflags" -S "$project" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DXWIN_DIR=$xwin" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_PREFIX_PATH=$sdk" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF "-DPGR4_NATIVE_SRC=$repo_root/src" "-DPGR4_REXGLUE_SDK_SRC=$sdk_src" \
    "-DPGR4_HOST_REXGLUE=$host_rexglue" "-DPGR4_FIDELITYFX_SDK=$ffx_src"
  cmake --build "$project/out" -j "$jobs"
  ls -l "$project/out/${title}_recompiled.exe"
}

game_stage() {
  need "$game/default.xex" "$game/gw.xex" "$host_rexglue" "$sdk/lib/cmake/rexglue/rexglueConfig.cmake"
  need_update
  step "fidelityfx checkout" ffx_checkout
  mkdir -p "$dest/game"
  local t excludes=()
  for t in "${titles[@]}"; do excludes+=("--exclude=/$t/generated" "--exclude=/$t/game-data" "--exclude=/$t/out"); done
  rsync -a --delete "${excludes[@]}" "$repo_root/games/" "$dest/game/"
  case_links "$dest/game" "$repo_root/src"
  # The MSVC CRT deprecates fopen, fscanf and friends in favor of its _s variants; the native
  # code uses the standard functions.
  local flags="$clang_cl_flags /D_CRT_SECURE_NO_WARNINGS" ldflags="" sum
  # Geometry Wars first, without the profile: it is PGR4's.
  step "game gw" build_title gw "$game/gw.xex"
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
  step "game pgr4" build_title pgr4 "$game/default.xex" "$update/default.xexp"
}

install_stage() {
  local runtime=$dest/run/runtime
  local files=("$dest/game/pgr4/out/pgr4_recompiled.exe" "$dest/game/gw/out/gw_recompiled.exe"
    "$sdk/bin/rexruntime.dll" "$sdk/bin/rexgpu-xenos.dll" "${ffx_dlls[@]}")
  need "${files[@]}"
  need_update
  mkdir -p "$runtime/title-update"
  # New files, not rewritten ones: a running game keeps the image it mapped, and a perf run's
  # hard links (deck-run.sh --perf) keep the modules it sampled.
  cp --remove-destination "${files[@]}" "$runtime/"
  cp -f "$update/default.xexp" "$update/Pgr4TitleUpdate.bin" "$runtime/title-update/"
  # Their linker maps, for deck-run.sh --perf (perf_symbols.py).
  local map
  for map in "$dest"/game/{pgr4,gw}/out/*_recompiled.map "$sdk_src"/out/win-amd64/{rexruntime,rexgpu-xenos}.map; do
    [[ -f $map ]] && cp --remove-destination "$map" "$runtime/"
  done
  ls -l "$runtime"
}
want sdk && step sdk sdk_stage
want game && step game game_stage
want install && step install install_stage
exit 0
