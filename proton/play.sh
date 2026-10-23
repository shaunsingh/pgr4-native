#!/usr/bin/env bash
# Plays PGR4 with the native renderer under Proton, from Steam (Game Mode or desktop) or a
# terminal.
#
#   play.sh                    play the installed build: the game alone, as on a console (its
#                              log keeps warnings and errors only)
#   play.sh --debug            a debugging session (PGR4_DEBUG=1 does the same): the game log
#                              with the native renderer's statistics and GPU pass breakdown every
#                              150 frames, the per-frame CSV (frame_times.py, native/tools/),
#                              deck_sampler.py's GPU load and clock, APU power, CPU load per
#                              thread and memory every 0.5 s, marks (F12) and the glitch catcher
#   play.sh --install [tree]   install $PGR4_BUILDS/native-<tree>/run/runtime (default: deck) as
#                              the build to play, so later rebuilds do not change it, and write
#                              the desktop entries Steam's "Add a Non-Steam Game" lists (one of
#                              them for debugging sessions)
#
# Everything lives under ~/pgr4-proton/play: runtime/ (the installed build), prefix/ (a Wine
# prefix of its own, so deck-run.sh never stops a game being played), userdata/ (the save, kept
# between sessions), logs/<stamp>-* (the last 30 sessions). In a debugging session a mark, F12,
# which a Steam Input binding can send from a back grip, writes the native front buffer to
# captures/f<frame>-native.png (the frame number matches the CSV) and the 5 s before it to
# logs/<stamp>-mark-<frame>.rec; <stamp>-glitch-<frame>.rec holds the frames around each
# whole-image glitch the renderer saw; with PGR4_RECORD=1 too, <stamp>-<frame>.rec holds every
# frame shown, 5 minutes a file, until 40 GB are left (native/tools/flicker.py and marks.py show
# them). In Steam, add the "PGR4 (native renderer)" shortcut without a compatibility tool: this
# script starts Proton itself.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
repo=$(cd "$here/.." && pwd)
top=$HOME/pgr4-proton/play
bench=$HOME/pgr4-proton
game=${PGR4_GAME:-$HOME/recomp/dumps/pgr4/game}
keep_sessions=30

if [[ ${1:-} == --install ]]; then
  tree=${2:-deck}
  src=${PGR4_BUILDS:-$HOME/recomp/builds/pgr4}/native-$tree/run/runtime
  [[ -f $src/pgr4_recompiled.exe ]] || { echo "no exe in $src" >&2; exit 1; }
  pgrep -f 'pgr4_recompiled\.exe' >/dev/null && { echo "the game is running; quit it first" >&2; exit 1; }
  mkdir -p "$top/runtime" "$top/userdata/cache" "$top/cache" "$top/logs" "$top/captures"
  rsync -a --delete "$src/" "$top/runtime/"
  # Start from the bench's prefix and caches: no Proton first-run setup, and the pipelines the
  # bench runs already compiled.
  [[ -d $top/prefix || ! -d $bench/prefix ]] || cp -a "$bench/prefix" "$top/prefix"
  [[ -d $bench/userdata/cache ]] && cp -an "$bench/userdata/cache/." "$top/userdata/cache/"
  [[ -d $bench/cache/native-$tree ]] && cp -an "$bench/cache/native-$tree/." "$top/cache/"
  apps=$HOME/.local/share/applications
  mkdir -p "$apps"
  entry() {
    cat > "$apps/$1.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$2
Comment=Project Gotham Racing 4 (ReXGlue recompilation) with the pgr4-native D3D12 renderer
Exec=$here/play.sh$3
Path=$top
Terminal=false
Categories=Game;
DESKTOP
  }
  entry pgr4-native-play 'PGR4 (native renderer)' ''
  entry pgr4-native-debug 'PGR4 (native renderer, debug)' ' --debug'
  git -C "$repo" log -1 --format='installed %h %s' > "$top/runtime/INSTALLED.txt" 2>/dev/null || true
  git -C "$repo" status --short >> "$top/runtime/INSTALLED.txt" 2>/dev/null || true
  echo "installed $src -> $top/runtime; desktop entries pgr4-native-play, pgr4-native-debug in $apps"
  exit 0
fi
debug=${PGR4_DEBUG:-0}
if [[ ${1:-} == --debug ]]; then debug=1; shift; fi
# A recording is only of use with the session's CSV and log.
if [[ ${PGR4_RECORD:-0} != 0 ]]; then debug=1; fi

runtime=$top/runtime
[[ -f $runtime/pgr4_recompiled.exe ]] || { echo "nothing installed: play.sh --install [tree]" >&2; exit 1; }
[[ -f $game/default.xex ]] || { echo "no game files in $game (set PGR4_GAME)" >&2; exit 1; }
pgrep -f 'pgr4_recompiled\.exe' >/dev/null && { echo "a pgr4_recompiled.exe is already running" >&2; exit 1; }
compat=$HOME/.local/share/Steam/compatibilitytools.d
shopt -s nullglob
protons=("$compat"/GE-Proton*/)
shopt -u nullglob
proton=${PROTON:-$(printf '%s\n' "${protons[@]}" | sort -V | tail -1)}
proton=${proton%/}
[[ -x $proton/proton ]] || { echo "no Proton at '$proton' (set PROTON=<dir with ./proton>)" >&2; exit 1; }

mkdir -p "$top/logs" "$top/prefix" "$top/userdata" "$top/cache" "$top/captures"
stamp=$(date +%Y%m%d-%H%M%S)
base=$top/logs/$stamp
win() { echo "Z:${1//\//\\}"; }   # Proton maps Z: to /

# Old sessions out, newest kept.
mapfile -t sessions < <(find "$top/logs" -maxdepth 1 -name '*-cmdline.txt' -printf '%f\n' | sort)
for ((i = 0; i + keep_sessions <= ${#sessions[@]}; i++)); do
  old=$top/logs/${sessions[i]%-cmdline.txt}
  rm -f "$old".* "$old"-*
done

# MTR's launcher arguments for 60 fps (launcher/Launcher.cs) with the guest's vblank at the Deck
# OLED panel's 90 Hz (PGR4_REFRESH=60 for an LCD Deck) and the native renderer as the presenter.
args=(--game_data_root "$(win "$game")" --user_data_root "$(win "$top/userdata")"
  --execute_unclipped_draw_vs_on_cpu --gpu_plugin=xenos --d3d12_present_vsync --xmp_music_gain 0.35
  --pgr4_60fps --pgr4_garage_walk_fix --d3d12_texture_shared_heaps --gpu_vblank_deadline_timer
  --gpu_wait_reg_mem_high_res --no-clear_memory_page_state --gpu_ring_wake_fast --texture_resource_reuse
  --texture_reuse_pool_limit_mib=256 --texture_cache_memory_limit_soft_lifetime=30
  --window_width=1280 --window_height=800 "--video_mode_refresh_rate=${PGR4_REFRESH:-90}"
  --pgr4_native --pgr4_native_primary --log_file "$(win "$base.log")")
# Game Mode (and any gamescope session): fullscreen; a desktop: a 1280x800 window.
if [[ ${XDG_CURRENT_DESKTOP:-} == gamescope || -n ${GAMESCOPE_WAYLAND_DISPLAY:-} ]]; then
  args+=(--fullscreen)
else
  args+=(--fullscreen=false)
fi
if [[ $debug != 0 ]]; then
  # The session record. A mark (F12) writes the native image alone, off the guest thread: the
  # Xenos copy only matters in oracle runs.
  args+=(--pgr4_native_gpu_timing "--pgr4_native_csv=$(win "$base-frames.csv")"
    "--pgr4_native_capture_dir=$(win "$top/captures")" --pgr4_native_capture_xenos=false
    "--pgr4_native_glitch_catch=$(win "$base-")" --guest_frame_stats --pgr4_d3d_stats_frames=150)
  export WINEDEBUG=${WINEDEBUG:-fixme-all} VKD3D_DEBUG=${VKD3D_DEBUG:-warn}
else
  args+=(--log_level=warn)
  export WINEDEBUG=${WINEDEBUG:--all} VKD3D_DEBUG=${VKD3D_DEBUG:-none}
  export VKD3D_SHADER_DEBUG=${VKD3D_SHADER_DEBUG:-none}
fi
# PGR4_RECORD=1: every frame shown, at half size, in files of 5 minutes (27000 frames at 90 fps,
# ~19 GB), for native/tools/flicker.py. Off by default: its ~50 MB/s of writes stall the game
# (every few seconds the kernel's writeback pauses the process's writers, the frame CSV and the
# log among them, for 0.1-1 s), and a mark's 5 s are in the glitch catcher's ring without it.
if [[ ${PGR4_RECORD:-0} != 0 ]]; then
  args+=("--pgr4_native_record=$(win "$base.rec")" --pgr4_native_record_chunk_frames=27000)
fi
args+=("$@")

export STEAM_COMPAT_DATA_PATH=$top/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
# As deck-run.sh: no xalia (Proton's gamepad-to-UI helper, ~10 % of a core during a race).
export PROTON_USE_XALIA=0
export VKD3D_SHADER_CACHE_PATH=$top/cache
cmd=("$proton/proton" run "$runtime/pgr4_recompiled.exe" "${args[@]}")
{ cat "$runtime/INSTALLED.txt" 2>/dev/null; echo "${cmd[*]}"; } > "$base-cmdline.txt"

if [[ $debug != 0 ]]; then
  python3 "$here/deck_sampler.py" record "$base-sampler.csv" &
  sampler=$!
  trap 'kill "$sampler" 2>/dev/null || true' EXIT
fi
echo "session $stamp$([[ $debug != 0 ]] && echo ' (debug)'): $base-*"
cd "$runtime"
status=0
"${cmd[@]}" > "$base-stdout.log" 2>&1 || status=$?
echo "game exited with $status at $(date +%T)" >> "$base-stdout.log"
exit "$status"
