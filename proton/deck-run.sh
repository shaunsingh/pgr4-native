#!/usr/bin/env bash
# One PGR4 race run under Proton on a Steam Deck (or any Linux machine with gamescope), the local
# counterpart of run.sh. Builds come from native/build.sh, the game files from
# ~/recomp/dumps/pgr4/game. Keeps its own prefix and user data under
# ~/pgr4-proton (a fresh save every run; vkd3d, Mesa and pipeline caches kept), drives the
# St Petersburg route with route_player.py, holds HOLD seconds after the race mark, stops the game
# and prints the frame rate over the 40 s after the mark (perf_fps.py), its frame-time
# distribution (frame_times.py: 1 % lows, hitches) and what limited it: GPU load, clock and APU
# power, and the busiest game threads (deck_sampler.py).
#
#   deck-run.sh <tree|runtime dir> <tag> [--mode primary|none|oracle] [--extra "<flags>"]
#               [--uncapped] [--hold 45] [--width 1280] [--height 800] [--fullscreen]
#               [--stretch 1.0] [--cpus 0-7] [--tap] [--session gamescope|desktop]
#               [--view window|sdl|headless] [--shots 0] [--perf 0] [--refresh 60]
#               [--output NAME]
#
# --session gamescope (the default) runs the game in gamescope at WIDTHxHEIGHT, the compositor of
# the Deck's Game Mode, whatever display the desktop has, and away from the desktop's keyboard
# focus: keys go in through XTEST. With --view window (the default) gamescope shows the game in a
# window on the desktop through its Wayland backend; --view sdl uses its SDL backend instead,
# which on a docked monitor with fractional scaling (5K at 2.25) often never shows its window, and
# gamescope then maps no game window either. --view headless shows nothing and so also leaves out
# the desktop compositor's work on that window.
# --session desktop opens the game's own window on the desktop and types through uinput. --shots N saves a gamescope screenshot every N seconds (0: one, 10 s
# after the race mark) into ~/pgr4-proton/shots-<tag>/, to check that the route kept in step.
# --refresh HZ sets the guest's vblank rate (video_mode_refresh_rate) and gamescope's refresh to
# HZ: 90 matches the Deck OLED's panel. With vsync on the game presents once per guest vblank.
# --output NAME (KDE Plasma, --view window) has a KWin script move gamescope's window to that
# output and make it fullscreen (kscreen-doctor -o lists the outputs; eDP-1 is the Deck's panel),
# so with an external monitor attached the game still shows on the panel at the panel's refresh.
# --perf N samples the game's user-space CPU time with perf for N seconds from the race mark and
# prints its hottest functions per thread (native/tools/perf_symbols.py, with the .map files of
# the runtime); sampling costs some speed, so such a run's frame rate is not a measurement.
#
# <tree> names $PGR4_BUILDS/native-<tree>/run/runtime (PGR4_BUILDS defaults to
# ~/recomp/builds/pgr4); a directory holding pgr4_recompiled.exe is used as is, so the released
# MTR-PGR4 runtime runs too (--mode none: it has no native renderer).
# --mode none runs Xenos alone; primary runs the native renderer as the presenter; oracle runs
# both and captures their front buffers 5, 10 and 15 s after the race mark (see run.sh).
# --stretch scales every delay of the route, for a machine that loads slower than the PC it was
# timed on. --cpus pins the game with taskset. Logs: ~/pgr4-proton/logs/.
set -euo pipefail
# The whole script is one block: bash parses it before running any of it, so editing this file
# during a run does not change the run.
{
tree=${1:?tree or runtime dir}; tag=${2:?tag}; shift 2
mode=primary; extra=""; uncapped=0; hold=45; width=1280; height=800; fullscreen=0; stretch=1.0
cpus=""; tap=0; session=gamescope; view=window; shots=0; perf_secs=0; refresh=60; output=""
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
    --tap) tap=1; shift ;;
    --session) session=$2; shift 2 ;;
    --view) view=$2; shift 2 ;;
    --shots) shots=$2; shift 2 ;;
    --perf) perf_secs=$2; shift 2 ;;
    --refresh) refresh=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
case $mode in primary|none|oracle) ;; *) echo "unknown mode $mode" >&2; exit 2 ;; esac
case $session in gamescope|desktop) ;; *) echo "unknown session $session" >&2; exit 2 ;; esac
case $view in window) backend=wayland ;; sdl) backend=sdl ;; headless) backend=headless ;; *) echo "unknown view $view" >&2; exit 2 ;; esac
[[ -z $output || $output =~ ^[A-Za-z0-9_-]+$ ]] || { echo "bad output name $output" >&2; exit 2; }
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
if [[ -f $tree/pgr4_recompiled.exe ]]; then
  runtime=$(cd "$tree" && pwd)
else
  runtime=${PGR4_BUILDS:-$HOME/recomp/builds/pgr4}/native-$tree/run/runtime
fi
game=${PGR4_GAME:-$HOME/recomp/dumps/pgr4/game}
keys=$repo/run/route-stpetersburg.keys
top=$HOME/pgr4-proton
compat=$HOME/.local/share/Steam/compatibilitytools.d
shopt -s nullglob
protons=("$compat"/GE-Proton*/ "$top"/proton/GE-Proton*/)
shopt -u nullglob
proton=${PROTON:-$(printf '%s\n' "${protons[@]}" | sort -V | tail -1)}
proton=${proton%/}
[[ -x $proton/proton ]] || { echo "no Proton at '$proton' (set PROTON=<dir with ./proton>)" >&2; exit 1; }
[[ -f $runtime/pgr4_recompiled.exe ]] || { echo "no exe in $runtime" >&2; exit 1; }
[[ -f $game/default.xex ]] || { echo "no game files in $game (set PGR4_GAME)" >&2; exit 1; }
pgrep -f 'pgr4_recompiled\.exe' >/dev/null && { echo "a pgr4_recompiled.exe is already running" >&2; exit 1; }
if [[ $session == desktop && ! -w /dev/uinput ]]; then
  echo "/dev/uinput is not writable: sudo setfacl -m u:$USER:rw /dev/uinput" >&2; exit 1
fi

# Started over SSH, the game still needs the desktop session's display.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DISPLAY=${DISPLAY:-:0}

mkdir -p "$top/logs" "$top/prefix" "$top/userdata" "$top/cache"
find "$top/userdata" -mindepth 1 -maxdepth 1 ! -name cache -exec rm -rf {} +   # fresh save, keep caches
stamp=$(date +%Y%m%d-%H%M%S)
base=$top/logs/pgr4-proton-$tag-$stamp
ilog=$top/logs/pgr4-co-input-$tag.log
rm -f "$ilog"
win() { echo "Z:${1//\//\\}"; }   # Proton maps Z: to /

# MTR's launcher arguments for 60 fps (launcher/Launcher.cs), then the measurement flags.
args=(--game_data_root "$(win "$game")" --user_data_root "$(win "$top/userdata")"
  --execute_unclipped_draw_vs_on_cpu --gpu_plugin=xenos --d3d12_present_vsync --xmp_music_gain 0.35
  --pgr4_60fps --pgr4_garage_walk_fix --d3d12_texture_shared_heaps --gpu_vblank_deadline_timer
  --gpu_wait_reg_mem_high_res --no-clear_memory_page_state --gpu_ring_wake_fast --texture_resource_reuse
  --texture_reuse_pool_limit_mib=256 --texture_cache_memory_limit_soft_lifetime=30
  "--window_width=$width" "--window_height=$height" "--video_mode_refresh_rate=$refresh" --mnk_mode
  --guest_frame_stats --pgr4_d3d_stats_frames=150 --log_file "$(win "$base.log")")
if [[ $fullscreen == 1 ]]; then args+=(--fullscreen); else args+=(--fullscreen=false); fi
[[ $tap == 1 ]] && args+=(--pgr4_d3d_tap)
[[ $mode == primary ]] && args+=(--pgr4_native --pgr4_native_primary)
# Per-frame times for frame_times.py (1 % lows, hitches).
[[ $mode != none ]] && args+=("--pgr4_native_csv=$(win "$base-frames.csv")")
capdir=$top/captures-$tag
if [[ $mode == oracle ]]; then
  rm -rf "$capdir"; mkdir -p "$capdir"
  args+=(--pgr4_native --readback_resolve=fast "--pgr4_native_capture_dir=$(win "$capdir")")
fi
[[ $uncapped == 1 ]] && args+=(--vsync=false --d3d12_present_vsync=false)
# shellcheck disable=SC2206
[[ -n $extra ]] && args+=($extra)

export STEAM_COMPAT_DATA_PATH=$top/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export PROTON_LOG=1 PROTON_LOG_DIR=$top/logs DXVK_LOG_LEVEL=warn VKD3D_DEBUG=warn
# No xalia (Proton's gamepad-to-UI helper for launchers): the game has no use for it and it costs
# ~10 % of a core during the race.
export PROTON_USE_XALIA=0
# One vkd3d-proton pipeline cache per build tree, outside the tree.
export VKD3D_SHADER_CACHE_PATH=$top/cache/$(basename "$(dirname "$(dirname "$runtime")")")
mkdir -p "$VKD3D_SHADER_CACHE_PATH"
cmd=("$proton/proton" run "$runtime/pgr4_recompiled.exe" "${args[@]}")
[[ -n $cpus ]] && cmd=(taskset -c "$cpus" "${cmd[@]}")
echo "${cmd[*]}" > "$base-cmdline.txt"

stop_game() {
  WINEPREFIX=$top/prefix/pfx "$proton/files/bin/wineserver" -k 2>/dev/null || true
  sleep 2
  pkill -9 -f 'pgr4_recompiled\.exe' 2>/dev/null || true
  # gamescope exits by itself once the game is gone, and ignores SIGTERM when it does not.
  if [[ -n ${gamescope:-} ]] && kill -0 "$gamescope" 2>/dev/null; then
    kill "$gamescope" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$gamescope" 2>/dev/null || break; sleep 0.2; done
    kill -KILL "$gamescope" 2>/dev/null || true
  fi
}
# The game process: of the processes that match, the one with the most threads (not Proton's
# launcher or a wrapper).
game_pid() {
  local p n best="" max=0
  for p in $(pgrep -f 'pgr4_recompiled\.exe'); do
    n=$(find "/proc/$p/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
    (( n > max )) && { max=$n; best=$p; }
  done
  echo "$best"
}
kwin_script=pgr4-place-output
cleanup() {
  [[ -n ${perfrec:-} ]] && kill -INT "$perfrec" 2>/dev/null || true
  stop_game
  [[ -n $output ]] && qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$kwin_script" >/dev/null 2>&1 || true
  [[ -n ${shooter:-} ]] && kill "$shooter" 2>/dev/null || true
  [[ -n ${player:-} ]] && kill "$player" 2>/dev/null || true
  [[ -n ${sampler:-} ]] && kill "$sampler" 2>/dev/null || true
  echo "stopped $(date +%T)"
  echo "log: $base.log"
  echo "input: $ilog"
}
trap cleanup EXIT
# Bash runs the EXIT trap after a signal only if the signal is handled; make it an exit.
trap 'exit 143' TERM
trap 'exit 130' INT
# Bash defers traps until a foreground command returns, so a plain `sleep 900` would hold off
# the cleanup (and kill a later run's game when it finally fires); waiting on a background
# sleep lets a signal through at once.
nap() { sleep "$1" & wait $!; }

player_args=(--script "$keys" --log "$ilog" --timeout 300 --width "$width" --height "$height"
  --stretch "$stretch")
shotdir=$top/shots-$tag
if [[ $session == gamescope ]]; then
  # gamescope starts its own Xwayland; the game's side writes that display's names for the
  # route player and the screenshots.
  envfile=$base-gamescope.env
  inner=$base-inner.sh
  { echo '#!/usr/bin/env bash'
    echo "printf 'export DISPLAY=%q GAMESCOPE_WAYLAND_DISPLAY=%q\\n' \"\$DISPLAY\" \"\$GAMESCOPE_WAYLAND_DISPLAY\" > '$envfile'"
    printf 'cd %q && exec' "$runtime"; printf ' %q' "${cmd[@]}"; echo
  } > "$inner"
  chmod +x "$inner"
  setsid gamescope --backend "$backend" -W "$width" -H "$height" -w "$width" -h "$height" -r "$refresh" \
    -- "$inner" > "$base-stdout.log" 2>&1 &
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
  player_args+=(--wm gamescope --input xtest)
  rm -rf "$shotdir"; mkdir -p "$shotdir"
  shoot() { gamescopectl screenshot "$shotdir/$1.png" >/dev/null 2>&1 || true; }
  if (( shots > 0 )); then
    ( n=0; while sleep "$shots"; do n=$((n + shots)); shoot "t$(printf %04d $n)"; done ) &
    shooter=$!
  fi
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
  python3 "$repo/native/tools/perf_symbols.py" "$base-stuck.data" "$base-stuck.maps" "${maps[@]}" \
    > "$base-stuck.txt" 2>&1 || true
  head -n 30 "$base-stuck.txt"; echo "stuck profile: $base-stuck.txt"
}
# Median draws over the last N frames of the CSV: a race draws 4000-5500 a frame after its start,
# the menus about 2000.
recent_draws() {
  tail -n "$1" "$base-frames.csv" 2>/dev/null | cut -d, -f2 | sort -n | awk '{a[NR]=$1} END {print NR ? a[int((NR+1)/2)] : 0}'
}

launched=$SECONDS; deadline=$((SECONDS + 600)); ok=0; startup_checked=0
while (( SECONDS < deadline )); do
  nap 2
  if grep -q '^\S\+ mark race' "$ilog" 2>/dev/null; then ok=1; break; fi
  if grep -q 'error' "$ilog" 2>/dev/null; then break; fi
  pgrep -f 'pgr4_recompiled\.exe' >/dev/null || { echo "GAME EXITED $(date +%T)"; break; }
  if [[ $mode != none && $startup_checked == 0 ]] && (( SECONDS - launched >= 45 )); then
    startup_checked=1
    if (( $(wc -l < "$base-frames.csv" 2>/dev/null || echo 0) <= 1 )); then
      echo "STARTUP HANG $(date +%T): no frame 45 s after launch"; sample_stuck; break
    fi
  fi
done
echo "route reached mark race: $ok $(date +%T)"
[[ $ok == 1 ]] || { tail -5 "$ilog" 2>/dev/null; exit 1; }
# The route's marks are timed from the window appearing, and a slower load starts the race some
# seconds after "mark race". The measurement starts when the frames look like a race: a median
# of 3000+ draws over the last 30 frames (the grid and the menus draw under 3000).
if [[ $mode != none ]]; then
  for (( waited = 0; waited < 30; ++waited )); do
    (( $(recent_draws 30) >= 3000 )) && break
    nap 1
  done
  draws=$(recent_draws 30)
  if (( draws < 3000 )); then
    echo "ROUTE OUT OF STEP $(date +%T): median $draws draws per frame 30 s after the race mark (a race draws 4000+)"
    exit 1
  fi
  echo "race running $(date +%T), $waited s after the route's race mark"
fi
race_iso=$(date +%Y-%m-%dT%H:%M:%S.%6N%:z)
race_hms=${race_iso:11:8}
if (( perf_secs > 0 )); then
  pid=$(game_pid)
  # The PE modules' load addresses, for perf_symbols.py.
  cp "/proc/$pid/maps" "$base-perf.maps"
  perf record -q -e cycles:u -F 999 -p "$pid" -o "$base-perf.data" -- sleep "$perf_secs" \
    > "$base-perf.out" 2>&1 &
  perfrec=$!
fi
if [[ $mode == oracle ]]; then
  # The game takes one capture per request file at its next Swap and deletes the file.
  for _ in 1 2 3; do nap 5; : > "$capdir/capture.request"; done
  nap $((hold > 15 ? hold - 15 : 0))
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
  shopt -s nullglob; module_maps=("$runtime"/*.map); shopt -u nullglob
  if (( ${#module_maps[@]} )); then
    python3 "$repo/native/tools/perf_symbols.py" "$base-perf.data" "$base-perf.maps" "${module_maps[@]}" \
      > "$base-perf.txt" 2>&1 || true
    head -n 40 "$base-perf.txt"; echo "perf profile: $base-perf.txt"
  else
    echo "perf: no .map files in $runtime (native/build.sh --stage install copies them)"
  fi
fi
fps=$(python3 "$repo/native/tools/perf_fps.py" "$base.log" "$race_hms" 40 2>&1 || true)
head -n 1 <<< "$fps"
[[ -s $base-frames.csv ]] && { python3 "$repo/native/tools/frame_times.py" "$base-frames.csv" "$base.log" "$race_hms" 40 || true; }
python3 "$here/deck_sampler.py" summary "$base-sampler.csv" "$race_hms" 40 || true
# For ab_compare.py: side,log,mark rows are built from these lines.
echo "RESULT $tag $base.log $race_iso $alive"
exit
}
