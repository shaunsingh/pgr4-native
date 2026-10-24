# Running the game on the Windows build host

The scripts in `lab/`, `run/` and `native/` run PGR4 on a Windows 10 or 11 PC that someone may
also be using as a desktop. The development PC was an i9-13900K with an RTX 3090. Each rule
below came from a failed run.

## Setup

- OpenSSH server, PowerShell 7 (`pwsh`), Python 3 with numpy and Pillow (`oracle_diff.py`,
  `compare_resolves.py`), ffmpeg on PATH for demo clips. The build toolchain is in the README.
- Everything lives under `C:\recomp` (`builds\pgr4`, `logs`, `locks`, `dumps\pgr4\game`). The
  scripts assume that path. If C: is small, use a junction: `mklink /J C:\recomp D:\recomp`.
- Stay logged in to the desktop with the screen unlocked, and run `lab` (or SSH in) as that
  same user. Games are started in that user's desktop session.
- Demo clips use `h264_nvenc`, so they need an NVIDIA GPU.
- `bench.ps1` (the Xenos baseline) needs PresentMon: pass `-PresentMon <exe>`, set
  `PRESENTMON`, or put it on PATH.

## Before starting anything

Look at the screen first (a screenshot over SSH, or walk over). Check the GPU:

```sh
fleet exec <host> "nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv"
lab pgr4 status      # queue, lock, running games, compilers
```

Do not launch while someone is using the PC (a foreground game or stream, typing, a busy GPU
you did not start).

## One game at a time: the lock

Game runs are serialized with `C:\recomp\locks\game.lock`:

- Create it with your name and the time before launching, and fail if it exists. Create it
  atomically (`[System.IO.File]::Open($lock, 'CreateNew', ...)`): two agents that check
  `Test-Path` and then write can both start a game.
- A lock older than 45 minutes whose game process is gone is stale and may be removed.
- Delete it right after the game is closed.
- `lab/co_run.ps1` does all of this through a FIFO queue (`C:\recomp\locks\queue`), and so does
  everything `lab pgr4` starts.

## Launching a GUI program over SSH

A plain command over SSH starts the process in session 0: no window, nothing to see or drive.
Start games through an interactive scheduled task in the desktop session, not elevated:

```powershell
schtasks /create /f /tn recomp-run /sc once /st 00:00 /ru $env:USERNAME /it /rl LIMITED `
  /tr "cmd /c cd /d C:\recomp\builds\pgr4\... && run.cmd > C:\recomp\logs\pgr4.log 2>&1"
schtasks /run /tn recomp-run
```

Without `/rl LIMITED` the game runs as administrator. Delete your tasks when done
(`schtasks /delete /f /tn ...`). `native/run.ps1` and the scripts in `lab/` and `run/` wrap all
of this. PGR4 does not always exit when asked; the scripts kill it after 5 s.

## Input: focus and key holds

- The input player (`run/input-player.ps1`) refuses to send keys unless the game window is in
  front. Windows' foreground lock can stop it taking focus from another app. Minimizing a
  foreground window you did not start is acceptable; closing it is not.
- Single key taps fall between the game's input polls. Hold each key about 250 ms, from a
  desktop scheduled task (`instrument/keys.ps1`).
- Routes need a fresh save; the run scripts delete it first.

## Windows Firewall prompts

Every new `pgr4_recompiled.exe` path triggers an "allow access" prompt on first launch. The
prompt takes the foreground and blocks the input player, so the run stalls at the title screen.
The game needs no network.

- Cancel: Windows adds an inbound Block rule for that exe.
- Allow: an inbound Allow rule.
- Before a new build tree's first run, add the rule yourself so no prompt appears:
  `New-NetFirewallRule -Direction Inbound -Action Block -Program <tree>\run\runtime\pgr4_recompiled.exe -DisplayName pgr4-<Name>`.
- `lab pgr4 trees` lists the trees and their rule. List all rules with
  `Get-NetFirewallApplicationFilter | ? Program -match 'recomp' | Get-NetFirewallRule`.

## Measuring

- Record resolution, frame cap, scene and build with every number.
- PresentMon needs elevation: run `run/bench.ps1` through an elevated SSH command, not a
  detached job (`fleet spawn` jobs are not elevated).
- Builds that present at a fixed rate regardless of the game make PresentMon numbers
  meaningless; use the game's own frame log (`native/tools/perf_fps.py`).
- ReXGlue v0.10.0's `--perf_log_csv` never opens its file; use PresentMon or the renderer's CSV.
- Uncapped runs: `--vsync=false --d3d12_present_vsync=false` (PGR4's game speed does not change).
- Image oracles (native against Xenos): run with `--readback_resolve=fast`; the capture tool
  holds a full readback for the compared frame.
- Steam Deck target: 1280x800. Proton runs: `docs/proton.md`.
- PGR4 runs go through `lab pgr4` (`lab pgr4 help`; `docs/perf-workflow.md` is the full guide).
  `lab pgr4 scripts` installs `lab/*.ps1` and `native/tools/*.py` into
  `C:\recomp\builds\pgr4\lab-scripts\perf\`, and `run/*.ps1` and `run/*.keys` into
  `...\lab-scripts\run\`.
- Give each agent a tree that already has a firewall rule, never a new tree.
- `fleet exec` sometimes answers "the Windows session lost this program's PowerShell output".
  Retry; if it repeats, prefix the command with `FLEET_WIN_SESSION=0`.

## Building

- At most 4-6 parallel compile jobs; RAM runs out first. Run long builds as detached jobs
  (`lab pgr4 build <tree> --wait` in the background).
- A build tree copied from another needs `out\pgr4\CMakeCache.txt` and `out\pgr4\CMakeFiles`
  deleted, then a full game rebuild. Tars made on macOS need `COPYFILE_DISABLE=1`, or CMake
  compiles the `._*.cpp` files.
- Keep generated code, traces and captures on the build host.
