# Examples from earlier waves

Real briefs, ownership tables and an integrator log from the sessions that built the renderer.
Machine names, people and private paths are removed; tree, agent and branch names are as they
were. Times are local wall-clock times.

## Wave 6 (2026-10-22): four agents after slice 0

Base: the slice-0 merge (register-as-local codegen, -17.9 % frame time). `native-TextureMod`
held the base exe as the shared A side for every `ab`. Deadline one hour after the spawn, new
game runs stopped ten minutes before it.

| Agent | Slice | Build tree | Files owned |
|---|---|---|---|
| RTFix | 2, render-thread wrong geometry | `native-DrawPerf` | `renderer.*`, `draw.*`, `geometry_shader.*` |
| TrackCost | 3, game-thread capture cost | `native-Integrator` | `pgr4_d3d_trace.cpp`, `d3d_state.*`, `profile.h` |
| GuestD3D | 4, replace the guest D3D draw path | `native-StateReader` | new files under `native/src/native/` (`guest_d3d.*`), hook-table entries |
| CodegenMore | the four remaining codegen options, plus one Xenos run of the base exe | `native-RTResolve` | `native/build.ps1` (the codegen-options block) |

The shared context was `context.md` filled in: base commit, deadline, current numbers (slice 0's
`ab` and oracle, game thread at ~98 % of a core, render thread ~45 %, `guest_d3d` 1.47 ms of a
9.27 ms frame), a budget of about 4 game runs per agent, `ab` against `TextureMod` with
`--b-ref HEAD --rounds 1`, and the oracle pass mark of each capture >= 30 dB and within ~1 dB of
31.4-32.2.

One of the four per-agent briefs, as sent:

```markdown
# Target
Perf plan slice 3: game-thread capture cost. `track` was 251 ns/draw (~0.8 ms/frame at 3,300
draws) in RenderThread's profile; the game thread is the bottleneck. Cut it.
Build tree: `native-Integrator` (old; first build `lab pgr4 build Integrator --sdk --wait`).
Files you own: `native/src/pgr4_d3d_trace.cpp`, `native/src/native/d3d_state.*`, `profile.h`.
GuestD3D will also need small hook-registration edits in `pgr4_d3d_trace.cpp`; accept them via
messages and keep its additions intact. RTFix may ask for `d3d_state` changes.

# Change
- First a profile of the base in your tree: `lab pgr4 profile Integrator trackcost-base --ref
  <base> --wait` (profiled run, stage ns/draw). Read where `track`/`read_state`/capture time
  goes on the game thread with the render thread on.
- Implement the plan's ideas where the profile says they pay: copy only changed register blocks
  using the guest device's dirty masks, skip `AfterCall` for hooked calls that flush nothing,
  smaller per-draw capture records. Behavior must stay identical (same draws, same state).
- Then `lab pgr4 profile Integrator trackcost-opt1 --wait` and one `ab` round vs TextureMod
  (`--tag trackcost-ab1`).

# Acceptance
- Stage numbers before/after (ns/draw), ab verdict with CI, oracle >= 30 dB on your final HEAD
  if budget allows (`trackcost-o1`).
- Branch `trackcost` pushed, STATE/worklog sections written, report.
```

The GuestD3D brief also said what to do if the work could not finish in time: commit it behind
a default-off flag (`--pgr4_native_replace_draws`) and report what works, what breaks with log
lines, and the next step.

## Session 4 (2026-10-21): four agents, all merged

| Agent | Branch, worktree | Build tree | Result | Merged |
|---|---|---|---|---|
| RenderThread | `rthread`, `../<repo>-rthread` | `native-DrawPerf` | render thread, 129 fps vs 96 on the same merged exe; oracle with it on 32.4/31.4/29.6 and 31.9/29.5/31.9 dB, so default off | last, after a rebase |
| DrawCPU | `drawcpu`, `../<repo>-drawcpu` | `native-StateReader` | memos, packed constants, cached CBV memory, whole-buffer index binding; owned stages ~6-9 % down | third |
| TexturePerf | `texperf`, `../<repo>-texperf` | `native-TextureMod` | texture stage 325 → 196 ns/draw (oracle 33.2/31.2/31.0) | second, fast-forward |
| HitchFix | `hitchfix`, `../<repo>-hitchfix` | `native-RTResolve` | pipeline cache + prewarm + library, dynamic depth bias, reserved upload blocks, background texture heap | first |

Integrator log, lightly edited:

- 15:00: the owner asked for at least three agents to keep optimizing. Spawned four, each with
  its own worktree, branch, build tree and file ownership. All four trees already had firewall
  rules, so no prompts.
- Before spawning: generalized one tree's lab scripts into the shared
  `C:\recomp\builds\pgr4\lab-scripts\perf\` (`co_run.ps1 -Dest`, atomic lock, compiler wait and
  report; `perf_build.ps1`, `perf_buildrun.ps1`, `perf_compare.ps1`).
- Fixes broadcast during the session: tar `native instrument` together (`build.ps1` prep needs
  the drawcount patch); `tar -xmf`, because restored mtimes from the controller let ninja skip
  edited sources; `FLEET_WIN_SESSION=0` when fleet loses PowerShell output; at 16:30 a FIFO
  ticket queue in `co_run.ps1`, because the 10 s lock poll starved one agent for more than
  20 minutes.
- ~17:00: HitchFix finished and merged. TexturePerf finished next, already rebased, and merged
  as a fast-forward. DrawCPU finished before 17:30 and merged with doc conflicts only. ~17:40:
  RenderThread finished; its merge conflicted in `draw.cpp` and `profile.h`, so the merge went
  back to RenderThread to rebase and resolve.
- 17:28-18:05: `int_chain.ps1 -Prefix int1` on the merged code in `native-CutOver`: primary
  10.00 ms/frame (100 fps), Xenos 7.69 ms (130 fps), oracle 31.6/31.4/30.7 dB, race start 76 ms
  cold / 84 ms warm, 3 frames >= 50 ms per run.

Three of the four first baselines were inflated by other agents' builds. Every run already
printed its compiler-process count; after this session `perf_build.ps1` also holds new builds
while a run has the lock. `docs/perf-workflow.md` now says to fix a structural refactor's merge
order before the spawn.

## Milestone 2: six owners building the first native frame

Before the renderer existed, the work was split by module. Each owner had files, a job and an
oracle it could run itself, and inputs that needed no game run (the traces from milestone 1).

| # | Owner | Files | Oracle (they run it) |
|---|---|---|---|
| 1 | Integrator | `renderer.*`, `d3d12_device.*`, hook dispatch, `build.ps1` | a cleared frame per guest Swap; frame count = Swap count; game screenshot unchanged |
| 2 | State | `d3d_state.*` | state equals the traced tap state at every draw of a menu and a race frame; 0 differences |
| 3 | Shaders | `shader_translate.*`, `shader_cache.*` | every shader in the menu and race traces translates and compiles; 5 spot checks against `--dump_shaders` |
| 4 | Textures | `textures.*` | 10 untiled menu textures byte-identical to the Xenos plugin's uploads |
| 5 | Render targets and resolves | `render_targets.*`, `resolve.*` | the 32 resolves of the menu frame match Xenos in size and format |
| 6 | Draw and present | `draw.*`, `present.*` | menu frame PSNR against Xenos > 30 dB |

Order: 1 and 2 first (2 is small and unblocks 6); 3, 4 and 5 in parallel against the traces;
6 integrates.
