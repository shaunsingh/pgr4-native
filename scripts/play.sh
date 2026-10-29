#!/usr/bin/env bash
# Plays PGR4 with the native renderer under Proton, from Steam (Game Mode or desktop) or a
# terminal.
#
#   play.sh                    play the installed build: the game alone, as on a console (its
#                              log keeps warnings and errors only), with OptiScaler loaded as
#                              fgmod (Decky Framegen's ~/fgmod) installs it, from runtime-fgmod/:
#                              a copy of the build with AMD's amdxcffx64.dll 4.1.2
#                              ($fsr4/amdxcffx64.dll) and fsr4-optiscaler.ini's settings in its
#                              OptiScaler.ini. The scene keeps the settings' antialiasing (MSAA
#                              unless the Options menu's Display page or F7 chose FSR); FSR is then
#                              FSR 4's Lite model. fgmod needs an OptiScaler that has the Lite model
#                              (master fd3a45a6 or later). Without ~/fgmod, or with --no-fgmod, the
#                              build plays from runtime/ without OptiScaler, and FSR is FSR 3.1
#   play.sh --debug            a debugging session (PGR4_DEBUG=1 does the same): the game log
#                              with the native renderer's statistics and GPU pass breakdown every
#                              150 frames, the per-frame CSV (frame_times.py, scripts/analysis/),
#                              deck_sampler.py's GPU load and clock, APU power, CPU load per
#                              thread and memory every second, marks (F12) and the glitch catcher,
#                              and a CPU profile of every PGR4 and Geometry Wars process
#   play.sh --install [tree]   install $PGR4_BUILDS/native-<tree>/run/runtime (default: deck) as
#                              the build to play, so later rebuilds do not change it, and write
#                              a desktop entry and a Steam shortcut (steam_shortcuts.py, with
#                              Steam closed) for each way to play: alone and --debug
#
# Flags after these go to the game (--pgr4_native_upscaler, --pgr4_native_frame_generation...)
# and override its settings for the session.
#
# Everything lives under ~/pgr4-proton/play: runtime/ (the installed build), prefix/ (a Wine
# prefix of its own: deck-run.sh stops only its own prefix's processes, so never a game being
# played), userdata/ (the save, kept between sessions), logs/<stamp>-* (the last 30 sessions).
# In a debugging session a mark, F12, which a Steam Input binding can send from a back grip,
# writes the native front buffer to captures/f<frame>-native.png (the frame number matches the
# CSV) and the 5 s before it to logs/<stamp>-mark-<frame>.rec; <stamp>-glitch-<frame>.rec holds
# the frames around each whole-image glitch the renderer saw (scripts/analysis/flicker.py and
# marks.py show them). A mark also logs the next 3 frames' draws to captures/draws-f<frame>.txt
# and checks them against guest memory (--pgr4_native_mark_audit; the findings go to the game
# log). The profile: logs/<stamp>-perf.data for the whole session, the maps of each title
# process and its profile in logs/<stamp>-perf-<exe>-<pid>.{maps,txt} (the .txt a minute or so
# after a long session), the modules as they ran in logs/<stamp>-perf-modules/, and
# logs/<stamp>-perf.clock, the wall clock and CLOCK_MONOTONIC (perf's clock here) at one instant,
# for perf_symbols.py --pid and --window. The Steam shortcuts --install writes have no
# compatibility tool: this script starts Proton itself.
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
repo=$(cd "$here/.." && pwd)
# shellcheck source=common.sh
source "$here/common.sh"
top=$HOME/pgr4-proton/play
bench=$HOME/pgr4-proton
game=${PGR4_GAME:-$HOME/recomp/dumps/pgr4/game}
fsr4=$bench/fsr4
keep_sessions=30

if [[ ${1:-} == --install ]]; then
  tree=${2:-deck}
  src=${PGR4_BUILDS:-$HOME/recomp/builds/pgr4}/native-$tree/run/runtime
  [[ -f $src/pgr4_recompiled.exe ]] || { echo "no exe in $src" >&2; exit 1; }
  check_no_game "the game is running; quit it first"
  mkdir -p "$top/runtime" "$top/userdata/cache" "$top/cache" "$top/logs" "$top/captures"
  rsync -a --delete --exclude='*.profraw' "$src/" "$top/runtime/"
  # The release's settings beside the executables, as a player's package has them; the flags
  # below override the few that differ.
  cp "$repo"/games/pgr4/pgr4_recompiled.toml "$repo"/games/gw/gw_recompiled.toml "$top/runtime/"

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
    # Steam's shortcut of the same name, with the flags as its launch options.
    local flag options=()
    for flag in $3; do options+=("\"$flag\""); done
    [[ $steam == 0 ]] || python3 "$here/steam_shortcuts.py" add "$2" "$here/play.sh" \
      --start-dir "$here/" --desktop "$apps/$1.desktop" --options "${options[*]}" > /dev/null
  }
  # Steam writes its shortcuts back when it exits, so they are updated only while it is closed.
  steam=1
  if pgrep -x steam > /dev/null; then
    steam=0
    echo "Steam is running: its shortcuts are left as they are (quit it and install again)" >&2
  fi
  entry pgr4-native-play 'PGR4 (native renderer)' ''
  entry pgr4-native-debug 'PGR4 (native renderer, debug)' ' --debug'
  # Entries retired 2026-10-27 and 2026-10-28 (FSR and frame generation are settings in the
  # game's Options menu): out of Steam's shortcuts and the applications menu, so the library
  # holds only the two above. Drop this once no install still has them.
  rm -f "$apps"/pgr4-native-{fsr4,framegen,fsr4-framegen}.desktop
  [[ $steam == 0 ]] || python3 "$here/steam_shortcuts.py" remove \
    'PGR4 (native renderer, FSR 4 Lite, debug)' 'PGR4 (native renderer, frame generation)' \
    'PGR4 (native renderer, FSR 4 Lite, frame generation, debug)'
  git -C "$repo" log -1 --format='installed %h %s' > "$top/runtime/INSTALLED.txt" 2>/dev/null || true
  git -C "$repo" status --short >> "$top/runtime/INSTALLED.txt" 2>/dev/null || true
  # The copy fgmod adds OptiScaler to at each start, which keeps the OptiScaler.ini it finds:
  # ~/fgmod's with fsr4-optiscaler.ini's settings.
  rsync -a --delete "$top/runtime/" "$top/runtime-fgmod/"
  [[ -f $fsr4/amdxcffx64.dll ]] && cp "$fsr4/amdxcffx64.dll" "$top/runtime-fgmod/"
  if [[ -f $HOME/fgmod/OptiScaler.ini ]]; then
    ini=$top/runtime-fgmod/OptiScaler.ini
    cp "$HOME/fgmod/OptiScaler.ini" "$ini"
    while IFS='=' read -r key value; do
      [[ $key =~ ^[A-Za-z0-9]+$ ]] && sed -i "s|^$key=.*|$key=$value|" "$ini"
    done < "$here/fsr4-optiscaler.ini"
  fi
  echo "installed $src -> $top/runtime (and runtime-fgmod); desktop entries pgr4-native-play" \
    "and pgr4-native-debug in $apps"
  exit 0
fi
debug=${PGR4_DEBUG:-0}
fgmod=1
while :; do
  case ${1:-} in
    --debug) debug=1 ;;
    --no-fgmod) fgmod=0 ;;
    *) break ;;
  esac
  shift
done

runtime=$top/runtime
if [[ $fgmod != 0 && -x $HOME/fgmod/fgmod && -d $top/runtime-fgmod ]]; then
  runtime=$top/runtime-fgmod
  [[ -f $runtime/amdxcffx64.dll ]] || echo "no amdxcffx64.dll in $runtime: FSR 4 falls back" >&2
else
  fgmod=0
fi
[[ -f $runtime/pgr4_recompiled.exe ]] || { echo "nothing installed: play.sh --install [tree]" >&2; exit 1; }
[[ -f $game/default.xex ]] || { echo "no game files in $game (set PGR4_GAME)" >&2; exit 1; }
check_no_game
find_proton
# The prefix's processes once no title runs in it: an earlier session's that outlived it.
end_prefix() {
  [[ -z $(prefix_title_pids "$top/prefix") ]] || return 0
  WINEPREFIX=$top/prefix/pfx "$proton/files/bin/wineserver" -k 2>/dev/null || true
  kill_prefix "$top/prefix"
}
end_prefix

mkdir -p "$top/logs" "$top/prefix" "$top/userdata" "$top/cache" "$top/captures"
stamp=$(date +%Y%m%d-%H%M%S)
base=$top/logs/$stamp

# Old sessions out, newest kept, a debugging session's perf-modules/ folder too.
mapfile -t sessions < <(find "$top/logs" -maxdepth 1 -name '*-cmdline.txt' -printf '%f\n' | sort)
for ((i = 0; i + keep_sessions <= ${#sessions[@]}; i++)); do
  old=$top/logs/${sessions[i]%-cmdline.txt}
  rm -rf "$old".* "$old"-*
done

# The release's runtime settings (also in runtime/*.toml) with the frame rate unlocked, the guest's vblank at the Deck OLED
# panel's 90 Hz (PGR4_REFRESH=60 for an LCD Deck) and the native renderer as the presenter.
game_args "$game" "$top/userdata" 60fps
args+=(--window_width=1280 --window_height=800 "--video_mode_refresh_rate=${PGR4_REFRESH:-90}"
  --pgr4_native --pgr4_native_primary --log_file "$(win "$base.log")")
# Game Mode (and any gamescope session): fullscreen; a desktop: a 1280x800 window.
if [[ ${XDG_CURRENT_DESKTOP:-} == gamescope || -n ${GAMESCOPE_WAYLAND_DISPLAY:-} ]]; then
  args+=(--fullscreen)
else
  args+=(--fullscreen=false)
fi
if [[ $debug != 0 ]]; then
  # The session record, its log at info (the release settings log errors only). A mark (F12)
  # writes the native image alone, off the guest thread: the Xenos copy only matters in oracle
  # runs.
  args+=(--pgr4_native_gpu_timing "--pgr4_native_csv=$(win "$base-frames.csv")"
    "--pgr4_native_capture_dir=$(win "$top/captures")" --pgr4_native_capture_xenos=false
    "--pgr4_native_glitch_catch=$(win "$base-")" --guest_frame_stats --pgr4_native_mark_audit=3
    --log_level=info)
  # err-hid: Wine logs every output report Steam's virtual pad gets (about 30 a second).
  export WINEDEBUG=${WINEDEBUG:-fixme-all,err-hid} VKD3D_DEBUG=${VKD3D_DEBUG:-warn}
else
  args+=(--log_level=warn)
  export WINEDEBUG=${WINEDEBUG:--all} VKD3D_DEBUG=${VKD3D_DEBUG:-none}
  export VKD3D_SHADER_DEBUG=${VKD3D_SHADER_DEBUG:-none}
fi
args+=("$@")

export STEAM_COMPAT_DATA_PATH=$top/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
# As deck-run.sh: no xalia (Proton's gamepad-to-UI helper, ~10 % of a core during a race).
export PROTON_USE_XALIA=0
# HDR10 output where gamescope's display takes HDR (Game Mode with HDR on): DXVK's DXGI offers the
# HDR10 color space only with DXVK_HDR=1. An SDR display stays SDR; DXVK_HDR=0 turns HDR off.
export DXVK_HDR=${DXVK_HDR:-1}
export VKD3D_SHADER_CACHE_PATH=$top/cache
cmd=("$proton/proton" run "$runtime/pgr4_recompiled.exe" "${args[@]}")
# fgmod copies OptiScaler in as dxgi.dll beside the executable it finds in the command, adds
# dxgi=n,b to WINEDLLOVERRIDES and runs the command.
[[ $fgmod != 0 ]] && cmd=("$HOME/fgmod/fgmod" "${cmd[@]}")
{ cat "$runtime/INSTALLED.txt" 2>/dev/null || true; echo "${cmd[*]}"; } > "$base-cmdline.txt"

# Debugging sessions: each title process's maps, which place its PE modules for perf_symbols.py,
# copied as it runs (a title launch starts a new process). A copy of an exited process's maps is
# empty and leaves the last one.
snapshot_maps() {
  local p name
  while :; do
    for p in $(prefix_title_pids "$top/prefix"); do
      name=$(grep -oEm1 '(pgr4|gw)_recompiled' "/proc/$p/maps" 2>/dev/null) || continue
      cat "/proc/$p/maps" > "$base-perf.tmp" 2>/dev/null && [[ -s $base-perf.tmp ]] &&
        mv "$base-perf.tmp" "$base-perf-$name-$p.maps"
    done
    nap 10
  done
}

if [[ $debug != 0 ]]; then
  python3 "$here/deck_sampler.py" record "$base-sampler.csv" --interval 1 &
  sampler=$!
  mkdir -p "$base-perf-modules"
  for module in "$runtime"/*.{exe,dll,map}; do
    [[ -e $module ]] && { ln -f "$module" "$base-perf-modules/" 2>/dev/null || cp "$module" "$base-perf-modules/"; }
  done
  python3 -c 'import time; print(f"{time.time():.6f} {time.clock_gettime(time.CLOCK_MONOTONIC):.6f}")' \
    > "$base-perf.clock"
  snapshot_maps &
  snapshotter=$!
  trap 'kill "$sampler" "$snapshotter" 2>/dev/null || true' EXIT
  # The session under perf, 249 Hz so hours stay small: the launcher, then, since a title the
  # last process launched outlives Proton's launcher, until no title runs. Every process the
  # session starts inherits the sampling, a title launch's and Wine's own. perf record -p would
  # spin a core once a thread it attached to exits (perf 7.1). -B skips the build-id pass at
  # the end, so a session Steam stops still closes its file at once.
  export PGR4_PREFIX=$top/prefix
  cmd=(perf record -q -B -k CLOCK_MONOTONIC -e cycles:u -F 249 -o "$base-perf.data" --
    bash -c 'source "$1"; shift; s=0; "$@" || s=$?
      while [[ -n $(prefix_title_pids "$PGR4_PREFIX") ]]; do sleep 1; done; exit "$s"' \
    session "$here/common.sh" "${cmd[@]}")
fi
echo "session $stamp$([[ $debug != 0 ]] && echo ' (debug)'): $base-*"
cd "$runtime"
status=0
"${cmd[@]}" > "$base-stdout.log" 2>&1 || status=$?
# A title launch (Geometry Wars from the garage, and back) ends the process Proton started.
# Outside Steam, Proton's launcher returns with it, so the session waits until no title runs
# (from Steam, the launcher waits itself; a debugging session's command above does).
[[ $debug != 0 ]] || while [[ -n $(prefix_title_pids "$top/prefix") ]]; do sleep 1; done
if [[ $debug != 0 ]]; then
  kill "$sampler" "$snapshotter" 2>/dev/null || true
  wait "$sampler" "$snapshotter" 2>/dev/null || true
  # Steam ends a session by signalling everything it started, this script too, so the profiles
  # are made in a unit of the user's systemd, outside the session, where there is one (systemd
  # expands $ in the command, and $$ is a $).
  report='for maps in "$1"-perf-*.maps; do
      [[ -s $maps ]] || continue
      pid=${maps%.maps}; pid=${pid##*-}
      python3 "$2/scripts/analysis/perf_symbols.py" "$1-perf.data" "$maps" "$1-perf-modules"/*.map \
        --pid "$pid" > "${maps%.maps}.txt" 2>&1
    done'
  systemd-run --user --collect --quiet -- bash -c "${report//\$/\$\$}" report "$base" "$repo" 2>/dev/null ||
    bash -c "$report" report "$base" "$repo" || true
fi
end_prefix
echo "game exited with $status at $(date +%T)" >> "$base-stdout.log"
exit "$status"
