# shellcheck shell=bash
# The parts of deck-run.sh and play.sh that must not drift apart; each sources this
# file. Expects `set -euo pipefail`.

# A Linux path as the game sees it: Proton maps Z: to /.
win() { echo "Z:${1//\//\\}"; }

# Sets proton to $PROTON, or else to the newest GE-Proton*/ in Steam's compatibilitytools.d and
# in ~/pgr4-proton/proton, compared by version (the directory name) whichever directory holds it;
# on a tie the one in ~/pgr4-proton/proton. Exits when that holds no ./proton.
find_proton() {
  local found=() d
  if [[ -n ${PROTON:-} ]]; then
    proton=$PROTON
  else
    shopt -s nullglob
    found=("$HOME"/.local/share/Steam/compatibilitytools.d/GE-Proton*/
      "$HOME"/pgr4-proton/proton/GE-Proton*/)
    shopt -u nullglob
    proton=$(for d in "${found[@]}"; do d=${d%/}; printf '%s\t%s\n' "${d##*/}" "$d"; done |
      sort -s -t $'\t' -k1,1V | tail -n 1 | cut -f 2)
  fi
  proton=${proton%/}
  [[ -x $proton/proton ]] || { echo "no Proton at '$proton' (set PROTON=<dir with ./proton>)" >&2; exit 1; }
}

# Exits when a pgr4_recompiled.exe runs, whoever started it: one game at a time.
check_no_game() {
  if pgrep -f 'pgr4_recompiled\.exe' >/dev/null; then
    echo "${1:-a pgr4_recompiled.exe is already running}" >&2; exit 1
  fi
}

# The pgr4_recompiled.exe processes (Proton's launcher and the game) started with
# STEAM_COMPAT_DATA_PATH=$1: every process of a Wine prefix inherits the variable, the ones Wine
# starts itself too, so a script stops its own game and never one started from another prefix.
prefix_game_pids() {
  local p
  for p in $(pgrep -f 'pgr4_recompiled\.exe'); do
    grep -qxzF "STEAM_COMPAT_DATA_PATH=$1" "/proc/$p/environ" 2>/dev/null && echo "$p"
  done
  return 0
}

# The processes of the Wine prefix $1 that run a title (pgr4_recompiled.exe, gw_recompiled.exe:
# one starts the other, game/common/title_launch.cpp): those that map the image itself, not
# Proton's launcher or steam.exe, whose command lines name it too.
prefix_title_pids() {
  local p
  for p in $(pgrep -f '(pgr4|gw)_recompiled\.exe'); do
    grep -qxzF "STEAM_COMPAT_DATA_PATH=$1" "/proc/$p/environ" 2>/dev/null &&
      grep -qE '(pgr4|gw)_recompiled\.exe$' "/proc/$p/maps" 2>/dev/null && echo "$p"
  done
  return 0
}

# Sends signal $1 to prefix_game_pids $2.
signal_prefix_games() {
  local pids
  mapfile -t pids < <(prefix_game_pids "$2")
  (( ${#pids[@]} == 0 )) || kill "-$1" "${pids[@]}" 2>/dev/null || true
}

# A fresh save: user data directory $1 emptied but for cache/ (the shader caches).
fresh_save() { find "$1" -mindepth 1 -maxdepth 1 ! -name cache -exec rm -rf {} +; }

# Sets args to the runtime settings of the release (native/release/pgr4_recompiled.toml) for the
# game files in $1 and the user data in $2; with $3 = 60fps, the frame rate unlocked.
game_args() {
  # shellcheck disable=SC2034  # read by the sourcing script
  args=(--game_data_root "$(win "$1")" --user_data_root "$(win "$2")"
    --execute_unclipped_draw_vs_on_cpu --gpu_plugin=xenos --d3d12_present_vsync
    --gpu_vblank_deadline_timer --gpu_wait_reg_mem_high_res --no-clear_memory_page_state
    --gpu_ring_wake_fast --texture_cache_memory_limit_soft_lifetime=30)
  if [[ ${3:-} == 60fps ]]; then args+=(--pgr4_60fps); fi
}

# Bash runs the EXIT trap after a signal only if the signal is handled: make TERM and INT exits.
exit_on_signals() {
  trap 'exit 143' TERM
  trap 'exit 130' INT
}
# Bash defers traps until a foreground command returns, so a plain `sleep 900` would hold off
# the cleanup (and kill a later run's game when it finally fires); waiting on a background
# sleep lets a signal through at once.
nap() { sleep "$1" & wait $!; }
