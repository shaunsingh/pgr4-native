# PGR4 under Proton (Linux, Steam Deck)

The Steam Deck target is the Windows build running under Proton, where vkd3d-proton turns D3D12
into Vulkan. There is no Linux build. Build on Windows, then run the same build tree under
Proton. The measurements below come from the Windows development PC (i9-13900K, RTX 3090)
booted into a Linux install with a Hyprland session, reading the build tree from the Windows
drive. Scripts: `proton/run.sh` and `proton/route_player.py`.

## What `run.sh` does

`proton/run.sh <tree> <tag> [--mode primary|none|oracle] [--extra "<flags>"] [--uncapped]
[--hold 45] [--height 720] [--cpus 16-23] [--tap]` runs one race:

1. Starts `<C>/recomp/builds/pgr4/native-<tree>/run/runtime/pgr4_recompiled.exe` with
   `proton run`, its own prefix in `~/pgr4-proton/prefix` and its own user data in
   `~/pgr4-proton/userdata` (a fresh save every run; the vkd3d and pipeline caches are kept).
2. `route_player.py` waits for the game window, floats it at 1280x720, and plays
   `route-stpetersburg.keys` through a uinput keyboard.
3. Holds `--hold` seconds after the race mark, stops the game and prints the frame rate over the
   40 s after the mark (`perf_fps.py`) and a `RESULT` line for `ab_compare.py`.

`--mode oracle` runs Xenos and native side by side and captures both front buffers 5, 10 and
15 s after the race mark into `~/pgr4-proton/captures-<tag>/`. `--cpus` pins the game with
`taskset`. Logs go to `~/pgr4-proton/logs/` in the same line formats as the Windows runs, so
`ab_compare.py` and `perf_fps.py` read them unchanged. A run takes about 3.5 minutes.

## Setup

1. **The build tree and the game files.** `run.sh` reads `$PGR4_C/recomp/builds/pgr4/...`,
   `$PGR4_C/recomp/dumps/pgr4/game` and the installed lab scripts under
   `$PGR4_C/recomp/builds/pgr4/lab-scripts/` (`PGR4_C` defaults to `/mnt/windows-c`). On a
   dual-boot PC, mount the Windows drive there. On a Deck, copy `<tree>\run\runtime`, the disc
   folder and `lab-scripts\` from the Windows PC into the same layout and point `PGR4_C` at it.
2. **Proton.** Unpack a GE-Proton release into `~/pgr4-proton/proton/` (`run.sh` takes the
   newest `GE-Proton*` there) or set `PROTON=<dir containing ./proton>`. Steam is not needed.
   GE-Proton 11-11 was used for the numbers below.
3. **Keyboard injection.** `route_player.py` creates a uinput keyboard, so the desktop user
   needs write access: `sudo setfacl -m u:$USER:rw /dev/uinput` (resets at reboot).
4. **A desktop session.** The game needs a logged-in graphical session; run `run.sh` over SSH
   as that desktop user.
5. **Python 3** for `route_player.py` and `perf_fps.py`. `oracle_diff.py` needs numpy and
   Pillow; without them, copy the captures to another machine.

`~/pgr4-proton` (Proton, prefix, caches, logs) took 1.9 GB.

## Desktop environment

`run.sh` launches the game inside a Hyprland session (`hyprctl dispatch`, with the Lua-config
dispatcher syntax of Hyprland 0.56), and `route_player.py` uses `hyprctl` to float and focus
the window. SteamOS Desktop Mode is KDE Plasma and Game Mode is gamescope, so on a Deck both
need a different launch (start `proton run` directly with the session's `DISPLAY` or
`WAYLAND_DISPLAY`) and the focus checks turned off. uinput input itself does not depend on the
compositor. This is not done yet.

## Results (2026-10-21)

Build tree `DrawPerf`, GE-Proton 11-11, NVIDIA 615.78.08, i9-13900K + RTX 3090, uncapped,
push-buffer tap on as in the Windows runs of that day. Frame time over the 40 s after the race
mark (`perf_fps.py`); the paired percentages come from `ab_compare.py` (4 s bins).

| Run | Proton ms/frame (fps) | Windows, same build |
|---|---|---|
| Native, render thread off | 10.99, 10.88 (91.0, 91.9) | 10.46-11.09 (90-96) |
| Native, render thread on | 8.65, 8.94 (115.6, 111.8) | 7.76-8.16 (123-129) |
| Xenos | 8.86, 8.77 (112.9, 114.1) | 7.95 (126) |
| Native, render thread on, 1280x800 window | 8.72 (114.7) | |
| Native, render thread on, CPUs 16-23 | 12.50 (80.0) | |
| Xenos, CPUs 16-23 | 13.96 (71.6) | |
| Native, render thread off, CPUs 16-23 | 17.69 (56.5) | |

- All cores: native with the render thread matches Xenos under Proton (-3.3 %, 95 % CI -10.1
  to +3.5 %). Without it, native is 17.5 % slower than Xenos (CI -23.2 to -11.7 %).
- Eight E-cores (CPUs 16-23, no SMT), a rough stand-in for a slower 8-thread CPU: native with
  the render thread is 14.7 % faster than Xenos (CI -23.6 to -5.9 %), and the render thread
  cuts native frame time by 30.8 % (CI -32.7 to -28.8 %).
- Against Windows, Proton makes Xenos 11 % slower and native with the render thread 8-13 %
  slower. Native without it is within 5 %.
- vkd3d-proton logged no errors, and the constant upload pool on CUSTOM write-back heap memory
  works under it.
- The 1280x800 window runs at the same speed as 1280x720.
- Image check (`--mode oracle`, render thread off, capped): 31.61, 31.50 and 31.68 dB against
  Xenos, as on Windows (31.3-31.9).
- One oracle run desynced from the route and captured the attract video (99 dB, both renderers
  showing the same movie frame). A 99 dB capture means "not in the race". Check the tap draws
  per frame in the log.
