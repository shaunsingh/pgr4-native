#!/usr/bin/env bash
# One PGR4 race run under Proton on a Steam Deck (or any Linux machine with gamescope). Builds
# come from scripts/build.sh, the game files from
# ~/recomp/dumps/pgr4/game. Keeps its own prefix and user data under
# ~/pgr4-proton (a fresh save every run; vkd3d, Mesa and pipeline caches kept), drives the
# St Petersburg route with route_player.py, holds HOLD seconds after the race mark, stops the game
# and prints the frame rate over the 40 s after the mark (perf_fps.py), its frame-time
# distribution (frame_times.py: 1 % lows, hitches) and what limited it: GPU load, clock and APU
# power, and the busiest game threads (deck_sampler.py).
#
#   deck-run.sh <tree|runtime dir> <tag> [--mode primary|none|oracle|release] [--extra "<flags>"]
#               [--uncapped] [--hold 45] [--width 1280] [--height 800] [--fullscreen]
#               [--stretch 1.0] [--cpus 0-7] [--fgmod] [--tap] [--session gamescope|desktop]
#               [--view window|sdl|headless] [--shots 0] [--perf 0] [--refresh 60]
#               [--display-refresh HZ] [--limit FPS] [--limit-at "SECS FPS"] [--output NAME]
#               [--captures "5 10 15"] [--catch]
#               [--keys FILE] [--race-draws 3000] [--draw-log FRAMES] [--pmu] [--server-trace]
#               [--hdr] [--hdr-heatmap]
#
# --session gamescope (the default) runs the game in gamescope at WIDTHxHEIGHT, the compositor of
# the Deck's Game Mode, whatever display the desktop has, and away from the desktop's keyboard
# focus: keys go in through XTEST. With --view window (the default) gamescope shows the game in a
# window on the desktop through its Wayland backend; --view sdl uses its SDL backend instead,
# which on a docked monitor with fractional scaling (5K at 2.25) often never shows its window, and
# gamescope then maps no game window either. --view headless shows nothing and so also leaves out
# the desktop compositor's work on that window.
# --session desktop opens the game's own window on the desktop and types through uinput. --shots N saves a gamescope screenshot every N seconds (0: one, 10 s
# after the race mark) into ~/pgr4-proton/shots-<tag>/, to check that the route kept in step; a
# route line "<at> mark shot <name>" saves <name>.png there when the player reaches it.
# --refresh HZ sets the guest's vblank rate (video_mode_refresh_rate) and gamescope's refresh to
# HZ: 90 matches the Deck OLED's panel. The native renderer's frame pacer starts the game's frames
# on the display's presents; with --pgr4_native_frame_latency=0 the game presents once per guest
# vblank instead.
# --display-refresh HZ gives gamescope a refresh other than the guest's, and --limit FPS sets
# gamescope's frame limit (a divisor of its refresh) the way Steam's frame limiter does in Game
# Mode, through its app refresh cycle override (gamescopectl debug_set_fps_limit):
# on the OLED a 60 fps limit switches the panel to 60 Hz (--display-refresh 60), a 30 fps one
# keeps 90 Hz and shows every third refresh (--limit 30). --limit-at "SECS FPS" changes the
# limit SECS seconds after the game started, as from Steam's menu during play; FPS 0 lifts it.
# --output NAME (KDE Plasma, --view window) has a KWin script move gamescope's window to that
# output and make it fullscreen (kscreen-doctor -o lists the outputs; eDP-1 is the Deck's panel),
# so with an external monitor attached the game still shows on the panel at the panel's refresh.
# --hdr runs gamescope with HDR on (--hdr-enabled, and --hdr-debug-force-support for a display
# that takes no HDR, which gamescope then shows tone mapped to SDR) and the game with DXVK_HDR=1,
# as Steam's Game Mode on an HDR display does, so the renderer presents HDR10. --hdr-heatmap
# also has gamescope show the luminance of every pixel as a heat map (--hdr-debug-heatmap).
# Screenshots (--shots) are then also saved as HDR10 AVIF; scripts/analysis/hdr_shot.py prints their
# luminance distribution in nits.
# --perf N samples the game's user-space CPU time with perf for N seconds from the race mark and
# prints its hottest functions per thread (scripts/analysis/perf_symbols.py, with the .map files of
# the runtime, linked with the modules into <log>-perf-modules/ for later analysis); sampling
# costs some speed, so such a run's frame rate is not a measurement.
# --pmu (with --perf) counts the front end over the same seconds, per thread: instructions per
# cycle, fetch stalls, instruction cache fills and iTLB misses (pmu_summary.py).
# --catch keeps the last 5 s of frames shown in memory and writes the frames around each
# whole-image jump to ~/pgr4-proton/logs/<session>-glitch-<frame>.rec (--pgr4_native_glitch_catch;
# scripts/analysis/flicker.py shows them), and those before each mark (F12, a route's "f12") to
# <session>-mark-<frame>.rec, as play.sh --debug does (scripts/analysis/marks.py).
# --keys FILE drives the game with that route_player.py script instead of scripts/routes/route-stpetersburg.keys;
# frame_times.py checks the run against the route's draw counts in FILE's .draws, if there is one.
# --race-draws N is the median draws per frame that shows the race is on (St Petersburg draws
# 4000+; a Quebec street race about 2000, its menus 1500-2000).
# --draw-log FRAMES writes the native renderer's log of every draw, clear and resolve of the
# FRAMES frames from the race mark on into ~/pgr4-proton/captures-<tag>/draws-f<frame>.txt
# (about 1.5 MB per race frame), for a structural diff of a glitch frame with its neighbours.
# --server-trace starts the wineserver with its request trace (-d1) and prints which threads call
# it how often over the 40 s after the race mark (server_requests.py); the trace slows the server,
# so such a run's frame rate is not a measurement.
#
# <tree> names $PGR4_BUILDS/native-<tree>/run/runtime (PGR4_BUILDS defaults to
# ~/recomp/builds/pgr4); a directory holding pgr4_recompiled.exe is used as is.
# --mode none runs Xenos alone; primary runs the native renderer as the presenter; oracle runs
# both and captures their front buffers 5, 10 and 15 s after the race mark, or at
# the seconds after the mark that --captures lists (ascending, fractions allowed).
# --mode release runs an unpacked release (scripts/release/package_release.py) as a player starts
# it: its own pgr4_recompiled.toml, and game/ (the title update as a package in game/Content/,
# or extracted in title-update/) and userdata/ beside the exe (a fresh save), with only the
# route's keyboard input and the measurement flags added.
# --stretch scales every delay of the route, for a machine that loads slower than the PC it was
# timed on. --cpus pins the game with taskset. --fgmod runs the game through ~/fgmod/fgmod, from
# runtime-fgmod/ beside the runtime dir as play.sh does: fgmod adds OptiScaler to the folder it
# runs and leaves it there, and Proton would load that dxgi.dll in every later run of the runtime.
# Logs: ~/pgr4-proton/logs/.
set -euo pipefail
# The whole script is one block: bash parses it before running any of it, so editing this file
# during a run does not change the run.
{
tree=${1:?tree or runtime dir}; tag=${2:?tag}; shift 2
mode=primary; extra=""; uncapped=0; hold=45; width=1280; height=800; fullscreen=0; stretch=1.0
cpus=""; tap=0; session=gamescope; view=window; shots=0; perf_secs=0; refresh=60; output=""
display_refresh=""; limit=0; limit_at=""
captures="5 10 15"; catch=0; keys=""; race_draws=3000; draw_log=0; pmu=0; server_trace=0; hdr=0
fgmod=0
while [[ $# -gt 0 ]]; do
  case $1 in
    --mode) mode=$2; shift 2 ;;
    --extra) extra=$2; shift 2 ;;
    --uncapped) uncapped=1; shift ;;
    --hold) hold=$2; shift 2 ;;
    --width) width=$2; shift 2 ;;
    --height) height=$2; shift 2 ;;
    --fullscreen) fullscreen=1; shift ;;
    --stretch) stretch=$2; shift 2 ;;
    --cpus) cpus=$2; shift 2 ;;
    --fgmod) fgmod=1; shift ;;
    --tap) tap=1; shift ;;
    --session) session=$2; shift 2 ;;
    --view) view=$2; shift 2 ;;
    --shots) shots=$2; shift 2 ;;
    --perf) perf_secs=$2; shift 2 ;;
    --refresh) refresh=$2; shift 2 ;;
    --display-refresh) display_refresh=$2; shift 2 ;;
    --limit) limit=$2; shift 2 ;;
    --limit-at) limit_at=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --captures) captures=$2; shift 2 ;;
    --catch) catch=1; shift ;;
    --keys) keys=$(readlink -f "$2"); shift 2 ;;
    --race-draws) race_draws=$2; shift 2 ;;
    --draw-log) draw_log=$2; shift 2 ;;
    --pmu) pmu=1; shift ;;
    --server-trace) server_trace=1; shift ;;
    --hdr) hdr=1; shift ;;
    --hdr-heatmap) hdr=2; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
case $mode in primary|none|oracle|release) ;; *) echo "unknown mode $mode" >&2; exit 2 ;; esac
case $session in gamescope|desktop) ;; *) echo "unknown session $session" >&2; exit 2 ;; esac
case $view in window) backend=wayland ;; sdl) backend=sdl ;; headless) backend=headless ;; *) echo "unknown view $view" >&2; exit 2 ;; esac
[[ -z $output || $output =~ ^[A-Za-z0-9_-]+$ ]] || { echo "bad output name $output" >&2; exit 2; }
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
# shellcheck source=common.sh
source "$here/common.sh"
if [[ -f $tree/pgr4_recompiled.exe ]]; then
  runtime=$(cd "$tree" && pwd)
else
  runtime=${PGR4_BUILDS:-$HOME/recomp/builds/pgr4}/native-$tree/run/runtime
fi
game=${PGR4_GAME:-$HOME/recomp/dumps/pgr4/game}
[[ $mode == release ]] && game=$runtime/game
keys=${keys:-$here/routes/route-stpetersburg.keys}
[[ -f $keys ]] || { echo "no key script $keys" >&2; exit 2; }
top=$HOME/pgr4-proton
find_proton
[[ -f $runtime/pgr4_recompiled.exe ]] || { echo "no exe in $runtime" >&2; exit 1; }
[[ -f $game/default.xex ]] || { echo "no game files in $game (set PGR4_GAME)" >&2; exit 1; }
check_no_game
if [[ $session == desktop && ! -w /dev/uinput ]]; then
  echo "/dev/uinput is not writable: sudo setfacl -m u:$USER:rw /dev/uinput" >&2; exit 1
fi

# Started over SSH, the game still needs the desktop session's display.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DISPLAY=${DISPLAY:-:0}

userdata=$top/userdata
[[ $mode == release ]] && userdata=$runtime/userdata
mkdir -p "$top/logs" "$top/prefix" "$userdata" "$top/cache"
# PGR4_KEEP_SAVE=1 keeps the profile (routes that need career progress, e.g. an owned car).
[[ ${PGR4_KEEP_SAVE:-} == 1 ]] || fresh_save "$userdata"
stamp=$(date +%Y%m%d-%H%M%S)
base=$top/logs/pgr4-proton-$tag-$stamp
ilog=$top/logs/pgr4-co-input-$tag.log
rm -f "$ilog"

# A release brings its own settings; other trees get the game's with the frame rate unlocked.
# Then the measurement flags.
if [[ $mode == release ]]; then
  # The reports read the renderer's frame lines, logged at info.
  args=(--log_level=info)
else
  game_args "$game" "$userdata" 60fps
  args+=("--window_width=$width" "--window_height=$height" "--video_mode_refresh_rate=$refresh")
  if [[ $fullscreen == 1 ]]; then args+=(--fullscreen); else args+=(--fullscreen=false); fi
fi
args+=(--mnk_mode --guest_frame_stats --log_file "$(win "$base.log")")
[[ $tap == 1 ]] && args+=(--pgr4_d3d_tap)
[[ $mode == primary ]] && args+=(--pgr4_native --pgr4_native_primary)
# Per-frame times for frame_times.py (1 % lows, hitches).
[[ $mode != none ]] && args+=("--pgr4_native_csv=$(win "$base-frames.csv")")
capdir=$top/captures-$tag
if [[ $mode == oracle ]]; then
  rm -rf "$capdir"; mkdir -p "$capdir"
  args+=(--pgr4_native --readback_resolve=fast "--pgr4_native_capture_dir=$(win "$capdir")")
elif (( draw_log > 0 || catch )); then
  rm -rf "$capdir"; mkdir -p "$capdir"
  args+=("--pgr4_native_capture_dir=$(win "$capdir")")
fi
(( draw_log > 0 )) && args+=("--pgr4_native_draw_log_frames=$draw_log")
[[ $uncapped == 1 ]] && args+=(--vsync=false --d3d12_present_vsync=false)
[[ $catch == 1 ]] && args+=("--pgr4_native_glitch_catch=$(win "$base-")")
# shellcheck disable=SC2206
[[ -n $extra ]] && args+=($extra)

export STEAM_COMPAT_DATA_PATH=$top/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export PROTON_LOG=1 PROTON_LOG_DIR=$top/logs DXVK_LOG_LEVEL=warn VKD3D_DEBUG=warn
# No xalia (Proton's gamepad-to-UI helper for launchers): the game has no use for it and it costs
# ~10 % of a core during the race.
export PROTON_USE_XALIA=0
(( hdr )) && export DXVK_HDR=1
# Steam hides the Deck's own controller (28de:1205) from the games it launches and hands them its
# virtual pad instead; without this, winebus reads the controller's HID interfaces at ~20 % of a
# core, a cost the game never pays when started from Steam. The route types on the keyboard.
export SDL_GAMECONTROLLER_IGNORE_DEVICES=${SDL_GAMECONTROLLER_IGNORE_DEVICES:-0x28de/0x1205}
# One vkd3d-proton pipeline cache per build tree, outside the tree, unless the caller set one (0:
# none, with MESA_SHADER_CACHE_DISABLE=true and --pgr4_native_pipeline_cache=false a cold start).
export VKD3D_SHADER_CACHE_PATH=${VKD3D_SHADER_CACHE_PATH:-$top/cache/$(basename "$(dirname "$(dirname "$runtime")")")}
[[ $VKD3D_SHADER_CACHE_PATH == 0 ]] || mkdir -p "$VKD3D_SHADER_CACHE_PATH"
if (( fgmod )); then
  rsync -a --delete --exclude=/game/ --exclude=/userdata/ --exclude=/logs/ --exclude='*.profraw' \
    --exclude='draws-*.txt' "$runtime/" "$runtime-fgmod/"
  runtime=$runtime-fgmod
fi
cmd=("$proton/proton" run "$runtime/pgr4_recompiled.exe" "${args[@]}")
[[ -n $cpus ]] && cmd=(taskset -c "$cpus" "${cmd[@]}")
(( fgmod )) && cmd=("$HOME/fgmod/fgmod" "${cmd[@]}")
echo "${cmd[*]}" > "$base-cmdline.txt"

stop_game() {
  WINEPREFIX=$top/prefix/pfx "$proton/files/bin/wineserver" -k 2>/dev/null || true
  sleep 2
  signal_prefix_games KILL "$top/prefix"
  kill_prefix "$top/prefix"
  # gamescope exits by itself once the game is gone, and ignores SIGTERM when it does not.
  if [[ -n ${gamescope:-} ]] && kill -0 "$gamescope" 2>/dev/null; then
    kill "$gamescope" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$gamescope" 2>/dev/null || break; sleep 0.2; done
    kill -KILL "$gamescope" 2>/dev/null || true
  fi
}
# The game process: of this prefix's processes that match, the one with the most threads (not
# Proton's launcher or a wrapper).
game_pid() {
  local p n best="" max=0
  for p in $(prefix_game_pids "$top/prefix"); do
    n=$(find "/proc/$p/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
    (( n > max )) && { max=$n; best=$p; }
  done
  echo "$best"
}
kwin_script=pgr4-place-output
cleanup() {
  [[ -n ${perfrec:-} ]] && kill -INT "$perfrec" 2>/dev/null || true
  [[ -n ${pmustat:-} ]] && kill -INT "$pmustat" 2>/dev/null || true
  stop_game
  [[ -n $output ]] && qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$kwin_script" >/dev/null 2>&1 || true
  [[ -n ${shooter:-} ]] && kill "$shooter" 2>/dev/null || true
  [[ -n ${limiter:-} ]] && kill "$limiter" 2>/dev/null || true
  [[ -n ${player:-} ]] && kill "$player" 2>/dev/null || true
  [[ -n ${sampler:-} ]] && kill "$sampler" 2>/dev/null || true
  [[ -n ${tracer:-} ]] && rm -f "$base-server.fifo"
  echo "stopped $(date +%T)"
  echo "log: $base.log"
  echo "input: $ilog"
}
trap cleanup EXIT
exit_on_signals

player_args=(--script "$keys" --log "$ilog" --timeout 300 --width "$width" --height "$height"
  --stretch "$stretch")
shotdir=$top/shots-$tag
if (( server_trace )); then
  # Started before the game, the server is the one its processes connect to; it exits with them
  # (stop_game kills it), which ends the recorder's input.
  rm -f "$base-server.fifo"; mkfifo "$base-server.fifo"
  python3 "$here/server_requests.py" record "$base-server.json" < "$base-server.fifo" &
  tracer=$!
  WINEPREFIX=$top/prefix/pfx WINEESYNC=1 WINEFSYNC=1 WINENTSYNC=1 \
    "$proton/files/bin/wineserver" -f -p30 -d1 > /dev/null 2> "$base-server.fifo" &
fi
if [[ $session == gamescope ]]; then
  # gamescope starts its own Xwayland; the game's side writes that display's names for the
  # route player and the screenshots.
  envfile=$base-gamescope.env
  inner=$base-inner.sh
  { echo '#!/usr/bin/env bash'
    echo "printf 'export DISPLAY=%q GAMESCOPE_WAYLAND_DISPLAY=%q\\n' \"\$DISPLAY\" \"\$GAMESCOPE_WAYLAND_DISPLAY\" > '$envfile'"
    # A title launch (Geometry Wars from the garage, and back) ends the process Proton started
    # and its launcher with it, and gamescope ends the session with its first child: the session
    # lasts until no title runs, as play.sh's does.
    printf 'source %q\n' "$here/common.sh"
    printf 'cd %q || exit; s=0;' "$runtime"; printf ' %q' "${cmd[@]}"; echo ' || s=$?'
    printf 'while [[ -n $(prefix_title_pids %q) ]]; do sleep 1; done; exit "$s"\n' "$top/prefix"
  } > "$inner"
  chmod +x "$inner"
  hdr_args=()
  (( hdr )) && hdr_args=(--hdr-enabled --hdr-debug-force-support)
  (( hdr == 2 )) && hdr_args+=(--hdr-debug-heatmap)
  setsid gamescope --backend "$backend" -W "$width" -H "$height" -w "$width" -h "$height" \
    -r "${display_refresh:-$refresh}" "${hdr_args[@]}" -- "$inner" > "$base-stdout.log" 2>&1 &
  gamescope=$!
  if [[ -n $output && $view == window ]]; then
    # gamescope's Wayland window has no app id and takes the game's title, so it is matched by
    # pid (setsid execs gamescope in the background job, so $gamescope is its pid). The game
    # opens its window seconds after this, which windowAdded catches.
    js=$base-kwin.js
    cat > "$js" <<EOF
function place(window) {
  if (!window || window.pid !== $gamescope) return;
  const output = workspace.screens.find((screen) => screen.name === "$output");
  if (!output) return;
  workspace.sendClientToScreen(window, output);
  window.fullScreen = true;
}
workspace.windowList().forEach(place);
workspace.windowAdded.connect(place);
EOF
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$kwin_script" >/dev/null 2>&1 || true
    if qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$js" "$kwin_script" >/dev/null 2>&1 &&
       qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1; then
      echo "KWin places the gamescope window on $output"
    else
      echo "KWin scripting unavailable: the window stays where KWin puts it" >&2
    fi
  fi
  for _ in $(seq 100); do [[ -s $envfile ]] && break; sleep 0.1; done
  [[ -s $envfile ]] || { echo "gamescope did not start (see $base-stdout.log)" >&2; exit 1; }
  # shellcheck disable=SC1090
  source "$envfile"
  # Steam's frame limiter sets gamescope's app refresh cycle override, which gamescope applies at
  # every paint (over --framerate-limit and the GAMESCOPE_FPS_LIMIT property); this command sets
  # the same override.
  if (( limit > 0 )); then
    timeout 10 gamescopectl debug_set_fps_limit "$limit" >/dev/null 2>&1 ||
      echo "gamescopectl debug_set_fps_limit $limit failed" >&2
  fi
  if [[ -n $limit_at ]]; then
    read -r limit_secs limit_fps <<< "$limit_at"
    ( sleep "$limit_secs"
      timeout 10 gamescopectl debug_set_fps_limit "$limit_fps" >/dev/null 2>&1 ||
        echo "gamescopectl debug_set_fps_limit $limit_fps failed" >&2
      echo "frame limit $limit_fps at $(date +%T)" ) &
    # Killed at the end: left running, it would set the frame limit of the next run's gamescope.
    limiter=$!
  fi
  player_args+=(--wm gamescope --input xtest)
  rm -rf "$shotdir"; mkdir -p "$shotdir"
  # gamescopectl can block for good (td-85: the periodic shots stopped after 24 s). With HDR,
  # also an HDR10 AVIF of the same frame (hdr_shot.py measures its luminance).
  shoot() {
    timeout 10 gamescopectl screenshot "$shotdir/$1.png" >/dev/null 2>&1 || true
    (( hdr )) && { timeout 10 gamescopectl screenshot "$shotdir/$1.avif" >/dev/null 2>&1 || true; }
    true
  }
  if (( shots > 0 )); then
    ( n=0; while sleep "$shots"; do n=$((n + shots)); shoot "t$(printf %04d $n)"; done ) &
    shooter=$!
  fi
  # A route line "<at> mark shot <name>" saves <name>.png when the player reaches it.
  ( tail --pid=$$ -n0 -F "$ilog" 2>/dev/null | while read -r _ kind word name _; do
      [[ $kind == mark && $word == shot ]] && shoot "$name"; done ) &
else
  (cd "$runtime" && exec setsid "${cmd[@]}" > "$base-stdout.log" 2>&1) &
  shoot() { :; }
fi
echo "game launched $(date +%T) ($mode, $session${gamescope:+ $view}, $runtime, $(basename "$proton"))"
python3 "$here/deck_sampler.py" record "$base-sampler.csv" &
sampler=$!
python3 "$here/route_player.py" "${player_args[@]}" &
player=$!

# Samples a game that stopped making progress, for the cause of an intermittent startup hang.
sample_stuck() {
  local pid; pid=$(game_pid); [[ -n $pid ]] || return 0
  cp "/proc/$pid/maps" "$base-stuck.maps"
  perf record -q -e cycles:u -F 999 -p "$pid" -o "$base-stuck.data" -- sleep 5 > /dev/null 2>&1 || true
  shopt -s nullglob; local maps=("$runtime"/*.map); shopt -u nullglob
  python3 "$repo/scripts/analysis/perf_symbols.py" "$base-stuck.data" "$base-stuck.maps" "${maps[@]}" \
    > "$base-stuck.txt" 2>&1 || true
  head -n 30 "$base-stuck.txt"; echo "stuck profile: $base-stuck.txt"
}
# Median draws over the last N frames of the CSV: a race draws 4000-5500 a frame after its start,
# the menus about 2000.
recent_draws() {
  tail -n "$1" "$base-frames.csv" 2>/dev/null | awk -F, '$2 ~ /^[0-9]+$/ {print $2}' | sort -n | awk '{a[NR]=$1} END {print NR ? a[int((NR+1)/2)] : 0}'
}
# Frames the native renderer drew (rows of the frame CSV): none while the Xenos plugin draws
# (--pgr4_native_classic until F9 takes the game back).
native_frames() { awk -F, 'NR > 1' "$base-frames.csv" 2>/dev/null | wc -l; }

launched=$SECONDS; deadline=$((SECONDS + 600)); ok=0; startup_checked=0
while (( SECONDS < deadline )); do
  nap 2
  if grep -q '^\S\+ mark race' "$ilog" 2>/dev/null; then ok=1; break; fi
  if grep -q 'error' "$ilog" 2>/dev/null; then break; fi
  pgrep -f 'pgr4_recompiled\.exe' >/dev/null || { echo "GAME EXITED $(date +%T)"; break; }
  if [[ $mode != none && $startup_checked == 0 ]] && (( SECONDS - launched >= 45 )); then
    startup_checked=1
    if (( $(wc -l 2>/dev/null < "$base-frames.csv" || echo 0) <= 1 )); then
      echo "STARTUP HANG $(date +%T): no frame 45 s after launch"; sample_stuck; break
    fi
  fi
done
echo "route reached mark race: $ok $(date +%T)"
[[ $ok == 1 ]] || { tail -5 "$ilog" 2>/dev/null; exit 1; }
# The route's marks are timed from the window appearing, and a slower load starts the race some
# seconds after "mark race". The measurement starts when the frames look like a race: a median
# of --race-draws (3000) or more over the last 30 frames (the grid and the menus draw fewer).
if [[ $mode != none ]] && (( $(native_frames) == 0 )); then
  echo "race mark $(date +%T): the Xenos plugin draws, so the race mark stands"
elif [[ $mode != none ]]; then
  for (( waited = 0; waited < 30; ++waited )); do
    (( $(recent_draws 30) >= race_draws )) && break
    nap 1
  done
  draws=$(recent_draws 30)
  if (( draws < race_draws )); then
    echo "ROUTE OUT OF STEP $(date +%T): median $draws draws per frame 30 s after the race mark (want $race_draws)"
    exit 1
  fi
  echo "race running $(date +%T), $waited s after the route's race mark"
  # The renderer polls this file every 15 frames; bit 16 starts the draw log.
  (( draw_log > 0 )) && echo 16 > "$capdir/draw-debug.txt"
fi
race_iso=$(date +%Y-%m-%dT%H:%M:%S.%6N%:z)
race_epoch=$(date -d "$race_iso" +%s)
race_hms=${race_iso:11:8}
if (( perf_secs > 0 )); then
  pid=$(game_pid)
  # The PE modules' load addresses, and the modules with their linker maps as they were sampled
  # (hard links: the next build installs new files), for perf_symbols.py and disassembly.
  cp "/proc/$pid/maps" "$base-perf.maps"
  mkdir -p "$base-perf-modules"
  shopt -s nullglob; modules=("$runtime"/*.exe "$runtime"/*.dll "$runtime"/*.map); shopt -u nullglob
  for module in "${modules[@]}"; do
    ln -f "$module" "$base-perf-modules/" 2>/dev/null || cp "$module" "$base-perf-modules/"
  done
  perf record -q -e cycles:u -F 999 -p "$pid" -o "$base-perf.data" -- sleep "$perf_secs" \
    > "$base-perf.out" 2>&1 &
  perfrec=$!
  if (( pmu )); then
    perf stat --per-thread -x, -o "$base-pmu.csv" -p "$pid" -e cycles:u,instructions:u \
      -e ic_fetch_stall.ic_stall_any:u,ic_cache_fill_l2:u,ic_cache_fill_sys:u \
      -e bp_l1_tlb_miss_l2_tlb_miss:u -- sleep "$perf_secs" > /dev/null 2>&1 &
    pmustat=$!
  fi
fi
if [[ $mode == oracle ]]; then
  # The game takes one capture per request file at its next Swap and deletes the file.
  waited=0
  for at in $captures; do
    nap "$(awk -v a="$at" -v w="$waited" 'BEGIN { d = a - w; print (d > 0 ? d : 0) }')"
    waited=$at; : > "$capdir/capture.request"
  done
  nap "$(awk -v h="$hold" -v w="$waited" 'BEGIN { d = h - w; print (d > 0 ? d : 0) }')"
  echo "captures:"; cat "$capdir/captures.csv" 2>/dev/null || echo "  none"
else
  nap 10; shoot race-10s
  nap $((hold > 10 ? hold - 10 : 0))
fi
alive=0; pgrep -f 'pgr4_recompiled\.exe' >/dev/null && alive=1
echo "alive at end: $alive $(date +%T)"
kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true; sampler=""
if [[ -n ${perfrec:-} ]]; then
  wait "$perfrec" 2>/dev/null || true; perfrec=""
  shopt -s nullglob; module_maps=("$base-perf-modules"/*.map); shopt -u nullglob
  if (( ${#module_maps[@]} )); then
    python3 "$repo/scripts/analysis/perf_symbols.py" "$base-perf.data" "$base-perf.maps" "${module_maps[@]}" \
      > "$base-perf.txt" 2>&1 || true
    head -n 40 "$base-perf.txt"; echo "perf profile: $base-perf.txt"
  else
    echo "perf: no .map files in $runtime (scripts/build.sh --stage install copies them)"
  fi
fi
if [[ -n ${pmustat:-} ]]; then
  wait "$pmustat" 2>/dev/null || true; pmustat=""
  python3 "$here/pmu_summary.py" "$base-pmu.csv" || true
fi
fps=$(python3 "$repo/scripts/analysis/perf_fps.py" "$base.log" "$race_hms" 40 2>&1 || true)
head -n 1 <<< "$fps"
# The route's per-second draw counts (frame_times.py --fingerprint), when it has them.
ref=(); [[ -f ${keys%.keys}.draws ]] && ref=(--ref "${keys%.keys}.draws")
[[ -s $base-frames.csv ]] && { python3 "$repo/scripts/analysis/frame_times.py" "$base-frames.csv" "$base.log" "$race_hms" 40 \
  "${ref[@]}" || true
  python3 "$repo/scripts/analysis/pacing.py" "$base-frames.csv" "$base.log" "$race_hms" 40 || true; }
python3 "$here/deck_sampler.py" summary "$base-sampler.csv" "$race_hms" 40 || true
if [[ -n ${tracer:-} ]]; then
  kill -USR1 "$tracer" 2>/dev/null || true; nap 2
  python3 "$here/server_requests.py" summary "$base-server.json" "$race_epoch" 40 || true
fi
# For ab_compare.py: side,log,mark rows are built from these lines.
echo "RESULT $tag $base.log $race_iso $alive"
exit
}
