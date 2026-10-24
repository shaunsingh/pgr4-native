# Measuring the PGR4 native renderer on Windows

How to build, run, profile and check the native renderer on the Windows build host, alone or with several agents
at once. Read `docs/running-on-windows.md` first (lock, focus, firewall, GUI launches over SSH) and
`docs/pgr4-native-architecture.md` for what the stages and counters mean. Numbers and history
are in `the development notes` (`#### Perf`, `#### TexturePerf`, `#### DrawCPU`,
`#### HitchFix`, `#### RenderThread`, `#### Integrator session 4`).

## The short way: `lab pgr4`

`lab pgr4 <verb>` (tools/lab/pgr4.ts) does each step below in one command from any worktree, so
nobody hand-writes tar/`fleet cp`/`fleet spawn`/sleep/tail chains again (in session 4 every
agent wrote its own: `tex_chain.ps1`, `hf_chain.ps1`, `rt_measure3.ps1`, `dc_oracle.ps1`).
Sources come from the git checkout of the current directory, or `--ref <commit>` (a clean
baseline of master without stashing).

```sh
lab pgr4 trees                                # which trees exist and have a firewall rule
lab pgr4 status                               # queue, lock, games, compilers, tasks, debug file
lab pgr4 build DrawPerf --sdk --wait          # first build of an old tree
lab pgr4 profile DrawPerf rt-base --ref master --wait    # baseline of master in your tree
lab pgr4 profile DrawPerf rt-opt1 --wait      # your working tree
lab pgr4 compare DrawPerf --modes primary --extra "--pgr4_native_render_thread=false" --wait
lab pgr4 oracle DrawPerf rt-oracle1 --wait    # PSNR vs Xenos through the queue
lab pgr4 out DrawPerf rt-opt1                 # a run's .out as clean text
lab pgr4 check CutOver int2 --wait            # integrator: build, cold+warm, oracle, compare
lab pgr4 ab DrawPerf --a-extra "--pgr4_native_render_thread=false" --b-extra "--pgr4_native_render_thread=true" --wait
lab pgr4 ab DrawPerf CutOver --a-ref master --b-ref HEAD --rounds 2 --wait   # two builds
lab pgr4 diff rt-oracle-on4                   # where native and Xenos differ in an oracle run
lab pgr4 demo CutOver --xenos --wait          # race clip, Xenos | native side by side + stills
lab pgr4 clean DrawPerf                       # after a dead wrapper (safe for other trees' runs)
```

Defaults since 2026-10-21: the push-buffer tap is off (`-Tap` turns it on) and the render thread
is on (`--pgr4_native_render_thread=false` gives the single-thread path, whose profile shows
every draw stage on the game thread). For an older run, the game log's line "PGR4 native:
recording on the render thread" tells whether it had the render thread on.

`--wait` blocks until the fleet job ends, then prints its tail. In omp, run it as a background
command (`bash` with `async: true`) and the result is delivered when the job finishes. Do not
guess `sleep N; fleet jobs tail`: in session 4 that woke early with "(no output)" again and
again. `lab pgr4 wait <job>` does the same for a job started without `--wait`. Set
`RECOMP_AGENT=<Name>` (or `--owner`) so queue tickets and the lock show who is running.

`lab pgr4 scripts` installs `lab/*.ps1` (the committed shared scripts) and
`native/tools/*.py` into `C:\recomp\builds\pgr4\lab-scripts\perf\`, and
`run/*.ps1` and `*.keys` (input player, route) into `...\lab-scripts\run\`, then
parse-checks them. Edit them in the repo, never only on the host: copies made by hand on the
host drifted during session 4.

## Build trees

Each agent gets its own tree under `C:\recomp\builds\pgr4\native-<Name>`. Use only trees whose
`run\runtime\pgr4_recompiled.exe` already has a Windows Firewall rule. A new exe path pops an
"allow access" prompt that steals focus from the input player, and the run stalls at the title
screen. Trees with a rule on 2026-10-21:

| Tree | Rule | Last used by |
|---|---|---|
| `native-CutOver` | Block | Integrator (integration checks, `int*` runs) |
| `native-DrawPerf` | Block | RenderThread |
| `native-StateReader` | Block | DrawCPU |
| `native-TextureMod` | Block | TexturePerf |
| `native-RTResolve` | Block | HitchFix |
| `native-RaceOracle` | Block | default `-Dest` of `rr_run.ps1` (the race oracle); do not rebuild |
| `native-Integrator` | Block | free |
| `native-DrawPresent`, `-TonemapFix`, `-MeshFix`, `-OracleFix` | Allow | free; allow rules approved by hand |

List them: `Get-NetFirewallApplicationFilter | ? Program -match 'native-' | % { $r = $_ |
Get-NetFirewallRule; '{0} {1}' -f $_.Program, $r.Action }`.

An old tree lacks the newest SDK patch, so its first build must be
`-Stage prep,sdk,game,install`. After that, `prep,game,install` takes about 2.5 minutes:
`prep` copies the new sources in, `game` regenerates and recompiles everything.

## Getting sources onto the host

Tar both `native` and `instrument` (`build.ps1` needs `..\instrument\rexglue-drawcount.patch`):

```sh
cd <worktree>/
COPYFILE_DISABLE=1 tar -cf /tmp/<branch>-native.tar native instrument
fleet cp /tmp/<branch>-native.tar <host>:'C:\recomp\builds\pgr4\native-<Name>\lab\perf-native.tar'
```

`perf_build.ps1` unpacks it with `tar -xmf`. The `-m` gives every file the extraction time. Plain
`tar -xf` restores the Mac mtimes, and an edited source older than its object file is silently
skipped by ninja (RenderThread got link errors for functions that were in the synced `.cpp`).

## Shared scripts

Source: `lab/` in this repo; installed copy: `C:\recomp\builds\pgr4\lab-scripts\perf\`
(`lab pgr4 scripts`), with copies of the Python tools from `native/tools/`. Every
script takes `-Dest <tree>`.

| Script | Does |
|---|---|
| `perf_build.ps1 -Dest [-Stage] [-NoGate]` | Unpacks `<tree>\lab\perf-native.tar` (`tar -xmf`; refuses a tar without `instrument\`), waits up to 15 min while a game run holds the lock (the build gate), runs `native\build.ps1`, log `<tree>\lab\perf-build.log` ending in `=== install done` |
| `co_run.ps1 -Dest -Tag [-Owner] [-Mode primary\|native\|none] [-Race] [-Uncapped] [-Extra] [-HoldSec 40] [-MeasureSec 20] [-RecordSec N -RecordOut x.mp4 [-RecordDelay 8]]` | One run: queue, lock, fresh userdata (keeps `userdata\cache`), start, input route (`-Race` St Petersburg, else the menu route), hold, per-thread CPU over the last 20 s, stop, release. `-RecordSec` records the game window (`record_window.ps1`) |
| `perf_buildrun.ps1 -Dest -Tag [-Owner] [-Extra] [-Stage] [-NoBuild]` | Build, one profiled uncapped primary race run, then `perf_stats.py` and `perf_hitch.py` |
| `perf_compare.ps1 -Dest [-Owner] [-Suffix] [-Extra] [-Modes primary,none]` | Unprofiled uncapped race runs, timed from the game log (`perf_fps.py`) |
| `oracle_q.ps1 -Dest -Tag [-Owner]` | The race oracle (`rr_run.ps1`, installed next to it) through the run queue |
| `int_chain.ps1 -Dest -Prefix [-NoBuild] [-Extra]` | Integrator check: build, cold profiled run (pipeline cache wiped), warm profiled run, oracle, primary and Xenos compare |
| `ab_run.ps1 -DestA [-DestB] [-ExtraA] [-ExtraB] [-ModeA] [-ModeB] [-Rounds 2] [-Tag ab] [-BuildA] [-BuildB]` | A/B bench: alternating unprofiled uncapped race runs (A,B then B,A), then `ab_compare.py` on `<DestA>\lab\<Tag>-runs.csv` |
| `demo_run.ps1 -Dest [-Tag demo] [-Seconds 20] [-Delay 8] [-Xenos] [-Uncapped] [-Extra]` | Recorded race runs into `C:\recomp\logs\pgr4-demo-<Tag>\`: `native.mp4`, with `-Xenos` also `xenos.mp4` and a labelled `sbs.mp4`; `still-25/50/75.png`; game fps while recording |
| `record_window.ps1 -InputLog -Out [-Delay] [-Seconds] [-Fps 60]` | Started by `co_run` as the interactive task `recomp-pgr4-co-record`: waits for the input log's `mark race` + Delay, records the game window's client area with ffmpeg `ddagrab` (Desktop Duplication) and `h264_nvenc`; log `<Out>.record.log` |

Run them as fleet jobs, never inline:
`fleet spawn <host> 'pwsh -NoProfile -File C:\recomp\builds\pgr4\lab-scripts\perf\perf_buildrun.ps1 -Dest C:\recomp\builds\pgr4\native-<Name> -Tag <tag> -Owner <Name>'`,
then `fleet jobs tail <host>:<id>` once, after the run had time (build ~2.5 min, run ~4-5 min plus
the queue). Output of a run also lands in `<tree>\lab\<tag>.out`; logs and CSVs in
`C:\recomp\logs\pgr4-native-<tag>-<stamp>{.log,-native.csv,-frames.csv,-stream.log}`.

## The run queue and the game lock

One game at a time. `co_run.ps1` and `oracle_q.ps1`:
1. Write a ticket `C:\recomp\locks\queue\<yyyyMMdd-HHmmss-fff>-<Owner>-<Tag>`; the oldest name
   goes first. Waiting runs touch their ticket every 10 s; any ticket untouched for 2 minutes
   belongs to a dead run and is removed by whoever sees it.
2. At the head of the queue, wait up to 6 minutes for no compiler on the host (`clang-cl`, `cl`,
   `ninja`, `lld-link`, `link`, `cmake`).
3. Create `C:\recomp\locks\game.lock` with `[System.IO.File]::Open(..., 'CreateNew')`. Atomic;
   two runs that test then write can both start a game. A lock older than 45 minutes whose game
   process is gone is stale.
4. Remove the ticket, run, then remove the lock in `finally`.

`rr_run.ps1` itself does not queue and throws at once if the lock is held. Use `oracle_q.ps1`.
Before the queue existed, a 10-second poll let other agents win the lock over and over (DrawCPU
waited more than 20 minutes). Runs started before 16:30 on 2026-10-21 used that poll.

Never kill a wrapper job that may already have launched the game: its `finally` is skipped and
the game, the input task and the lock stay behind. If that happens: `native\run.ps1 -Action
stop -Dest <tree>`, `schtasks /end` and `/delete` the `recomp-pgr4-co-input` task, remove the
lock.

## Reading the numbers

- Compare stages in ns per draw from `perf_stats.py` (steady frames: no new PSO, under 256 KB of
  vertex uploads), not whole-run fps. Two primary runs of the same exe differed by 8 %, and the
  last-600-frame window lands on 2,000-5,000-draw scenes depending on how fast the run was.
- `read_state` (~18-20 ns per draw) is the machine-speed reference. If it is 25+ ns, the box was
  busy.
- `co_run` prints `compiler processes at hold start / end`. Nonzero means another agent was
  building; every stage is 15-20 % high. Rerun.
- Frame time from the game log (`perf_fps.py <log> <mark HH:MM:SS> 40`) is the honest fps
  number: the 40 s after the race mark, from the `PGR4 D3D: frames N` lines.
- To decide whether a change is faster, use `lab pgr4 ab`, not two separate runs. It alternates
  A and B runs and compares frame time in 4 s bins after the input log's exact `mark race`
  time. The route replays by time, so a bin is about the same scene in every run; the draw
  counts per bin in its table show whether that held. The verdict is "B faster/slower" only
  when the 95 % interval of the paired per-bin difference excludes zero. One round (two runs,
  ~7 min) gave an interval of about ±3 %: render thread on vs off in DrawPerf on 2026-10-21 was
  -26.3 % frame time (-29.3 to -23.2 %), 90 → 123 fps, with per-bin draw counts within 5 %.
  For changes of a few percent, use `--rounds 2` or more.
- `perf_hitch.py <csv> [min_ms] [min_draws]` lists slow frames with their top stages, texture
  counters and `pso_pending`, plus run totals. `min_draws` 100 leaves out title-screen frames.
- `perf_race.py <csv>` (TexturePerf) prints race-window stage and texture-binding medians.
- A cold run (no `userdata\cache\pgr4-native`) compiles every pipeline; a warm run loads them.
  `int_chain.ps1` does both.

## Image check

The race oracle runs Xenos and native side by side (`--pgr4_native`, `--readback_resolve=fast`),
captures the front buffer at 3 race frames and prints PSNR in dB (last column of `captures:`).
Pass mark: each capture ≥ 30 dB and within ~1 dB of the branch's own baseline. Reference values
on 2026-10-21: 31.0-33.2 dB depending on the frames captured. The car's rear spoiler is missing
in native; that is a known bug and does not move these numbers much.

`lab pgr4 diff <oracle tag>` shows where the frames differ. It runs `oracle_diff.py` on the host
over `C:\recomp\logs\pgr4-<tag>-captures` and copies to `.cache/pgr4/diff/<tag>/`:
`f<frame>-diff.png` (native | Xenos | |difference| x8 heatmap, the 8 worst 80x80 tiles outlined),
`f<frame>-worst.png` (full-size crops around the worst tile) and `summary.txt` (PSNR per tile).
One wrong draw shows as a few tiles far below the rest; a tonemap or gamma change lowers every
tile. In a normal run the worst tiles are the player's car (missing spoiler, motion) at 17-19 dB
and the median tile is ~46 dB.

## Demo clips

`lab pgr4 demo <tree> [--xenos] [--seconds 20] [--delay 8] --wait` records the race for sharing.
`co_run` starts `record_window.ps1` as an interactive task next to the input player. It waits
for the input log's `mark race`, then records the game window from `--delay` seconds after it,
so a native run and a Xenos run (`--xenos`) start at the same point of the route and line up in
the side-by-side. Recording uses Desktop Duplication (`ddagrab`, picked by matching the
screen size) and NVENC, so it costs the game little CPU; run without `--uncapped` for what a
player sees. The run prints the game's frame rate over the recorded stretch. `--wait` copies
the main clip and the stills to `.cache/pgr4/demo/<tag>/`; `lab pgr4 demo-get <tag>` does it
later. Clips stay in `C:\recomp\logs\pgr4-demo-<tag>\`.

First run (2026-10-21, `demo DrawPerf --xenos --tag t1`, two runs, 15 min with the queue): 20 s
clips of 1280x720 at 60 fps, 38 MB each, `sbs.mp4` 75 MB; the race clocks in the side-by-side
differ by 30 ms (0:18.33 and 0:18.36). The cars' lines drift apart over the race because the
physics is not deterministic between runs, so cut demos from the first seconds after the mark.
ddagrab delivers D3D11 frames; do not add `-pix_fmt` to the NVENC command (ffmpeg cannot
insert a software conversion after a d3d11 source and writes nothing).

## Running several agents

- Own worktree and branch each (`git worktree add ../pgr4-native-<name> -b <name>`), own tree on
  the host, own `#### <Name>` section in STATE.md and `## <date> <Name>` in the worklog.
- File ownership agreed up front; changes outside it are minimal and listed in the report.
- Agents message each other and the integrator with `write agent://<Name>`; the integrator
  broadcasts tool changes with `write agent://all`.
- The integrator merges branches into master, runs `lab pgr4 check CutOver <prefix> --wait` on
  the merged code, and updates `## Current status`.
- Merging: the worklog has `merge=union` (.gitattributes), so appended worklog
  sections merge by themselves. STATE.md tail conflicts, where both branches appended a
  section, are resolved by `tools/resolve-append-conflicts.py <path>/STATE.md`. It
  keeps both and leaves any real conflict in place.
- A structural refactor (like the render thread) conflicts with every micro-optimization in
  the same files. Plan it: either it merges first and the others rebase onto it, or it merges
  last and its own agent rebases and resolves (session 4 did the latter). Either way, tell
  the agent up front.
- Agents must baseline their own tree first (`lab pgr4 profile <tree> <t> --ref master`). Then
  `perf_buildrun`'s compiler report tells them whether to rerun; in session 4, three of four
  first baselines were inflated by other agents' builds. The build gate in `perf_build.ps1`
  now holds new builds while a run has the lock.
- Expect lock waits: with four agents each run waited 5-25 minutes. Queue one run per agent at
  a time.

## Troubleshooting

| Symptom | Cause, fix |
|---|---|
| `fleet exec` returns "the Windows session lost this program's PowerShell output" | Retry; if repeated, prefix `FLEET_WIN_SESSION=0` |
| `fleet spawn` says "launch unconfirmed ... Cannot access a closed Stream" and exits 1 | The job usually did start (check `fleet jobs log <job>`; it showed the build running). Never retry blind, or two runs start. `lab pgr4` sets `FLEET_WIN_SESSION=0` and checks the log itself |
| Build stops at prep: missing `..\instrument\rexglue-drawcount.patch` | Tar `native instrument`, not only `native` |
| Link errors for functions that exist in the synced `.cpp` | Stale mtimes; `perf_build.ps1` now extracts with `-m`, rebuild once |
| `perf_buildrun` reports a failed build after `-Stage sdk` | Fixed: the check now matches `): error` and `error LNK` only |
| `-Modes primary,none` runs neither mode | Fixed in `perf_compare.ps1` (splits commas); older copies need one mode per call |
| Run stalls at the title screen | A firewall prompt or another window has focus; take a screenshot (`fleet shot <host>`) |
| Game dies in the intro with `frontend_mad.bpr` and "Call to invalid or unregistered function at 0xF8000098" | Seen once (DrawCPU, `-NoBuild` rerun); looked environmental, not reproduced |
