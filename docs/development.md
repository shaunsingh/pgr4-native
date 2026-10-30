# Development

This document covers building, running, debugging and measuring the renderer, how the SDK
patches are kept, and the performance rules the renderer follows.
[architecture.md](architecture.md) explains how the renderer works.

## Layout

| Path | Contents |
|---|---|
| `src/` | The renderer, with the D3D hooks (`pgr4_d3d_trace.cpp`, `d3d_surface.inc`) and `native/` |
| `games/` | The game projects `pgr4/` and `gw/` (codegen manifest, CMake, app, runtime settings, and PGR4's own UI scripts in `pgr4/ui/`), and `common/` for the title handoff between them |
| `patches/` | `rexglue.lock`, which names the ReXGlue commit, and the SDK patches, applied in order |
| `scripts/` | Build, play and benchmark scripts and samplers, with `analysis/` (log, CSV and capture tools), `routes/` (scripted inputs) and `release/` (packaging) |
| `cmake/` | The clang-cl cross toolchain file |
| `tests/shader_xlate/` | Offline shader translation test |
| `docs/` | This file and the architecture |

Game data and builds stay outside the repository. The disc's files go in
`~/recomp/dumps/pgr4/game` and Title Update #2 in `~/recomp/dumps/pgr4/title-update`. Build
trees live under `~/recomp/builds/pgr4/native-<tree>`, and Proton prefixes and logs under
`~/pgr4-proton`.

## Setup

The build runs on Linux and cross-compiles Windows x64 executables with clang-cl and lld-link,
which then run under Proton. It needs clang, lld, the llvm tools, CMake, Ninja, Python 3, Git,
curl and a Lua interpreter (5.1 or later, or LuaJIT, for the Fennel compiler). When `clang-cl`
is not on `PATH`, `scripts/build.sh` runs itself in a distrobox (`pgr4dev` by default, or
`$PGR4_DISTROBOX`).

1. Run `scripts/fetch-xwin.sh` to splat the MSVC CRT and Windows SDK at pinned versions into
   `~/recomp/xwin`. Running it accepts Microsoft's license terms for those files.
2. Extract your own disc into a folder (`default.xex`, `gw.xex`, `Game\`, `UI\` and the rest).
3. Get Title Update #2 (0.0.2.1), either as its package `TU_16L61VP_0000004000000.00000000000G1`
   or as its two files. The build checks them against `games/pgr4/title-update.sha256`, since the
   code is generated for that update only.

| File | SHA-256 |
|---|---|
| package `TU_16L61VP_0000004000000.00000000000G1` | `9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8` |
| `default.xexp` | `7e7c672b542dc20d20bed72e27603335701a6eb9f243d2a8a2468bb966ee9401` |
| `Pgr4TitleUpdate.bin` | `f4bc47545df5fe8d0301bd4593d3b7b6e3871520f98e5f7249921973781a4b74` |

## Build

```sh
scripts/build.sh --game <disc folder> --update <title update package or folder>
scripts/build.sh --stage game,install    # rebuild only the game projects and the renderer
```

The `sdk` stage clones ReXGlue at the commit `patches/rexglue.lock` names, applies `patches/`,
builds the codegen tool and cross-builds the SDK. The `game` stage generates and builds
`games/pgr4` (with `src/`) and `games/gw`. The `install` stage writes both executables, the
ReXGlue DLLs and `title-update/` to `<dest>/run/runtime`. The header of `scripts/build.sh`
describes every option, and these are the common ones.

- `--dest DIR` sets the build tree (default `~/recomp/builds/pgr4/native-deck`). Use one tree per
  worktree when several people build at once.
- `--march` and `--mtune` default to `x86-64-v3` and `znver2`, the Deck's Zen 2. The build also
  prefers 128-bit vectors, because without a Zenbleed microcode fix the kernel sets a chicken bit
  that makes `VZEROUPPER` slow.
- `--pgo gen` builds the game instrumented. A run then writes `pgr4_recompiled.profraw` beside
  the executable every few seconds, and `llvm-profdata merge -o <dest>/pgo/default.profdata`
  makes it the default profile. A profile goes stale only in changed functions (clang warns per
  file), so retake it before a release when hot code has changed.
- `--force` rebuilds the codegen tool and regenerates all game code.

Installing writes new files rather than overwriting the old ones, so a running game keeps the
modules it loaded.

Format C++ with the root `.clang-format` (ReXGlue's Google style at 100 columns) by running
`distrobox enter pgr4dev -- clang-format --style=file -i <files>`. Layout-only commits go in
`.git-blame-ignore-revs`.

## Run

```sh
scripts/play.sh --install [tree]    # install a build as the one to play and write the desktop entries
scripts/play.sh                     # play, with only warnings and errors in the log
scripts/play.sh --debug             # play a debugging session (PGR4_DEBUG=1 does the same)
scripts/deck-run.sh <tree> <tag> --refresh 90 --hold 45 --output eDP-1   # scripted race with timings
```

`play.sh --install` writes two desktop entries, "PGR4 (native renderer)" and its debugging
variant, which Steam's "Add a Non-Steam Game" lists. Add them without a compatibility tool, since
the script starts Proton itself. Everything it uses lives under `~/pgr4-proton/play`, in
`runtime/`, `prefix/`, `userdata/`, `logs/` and `captures/`. With Decky Framegen's `~/fgmod`
installed, both play `runtime-fgmod/`, a copy of the build with AMD's `amdxcffx64.dll` 4.1.2,
through fgmod, so OptiScaler is loaded and the upscaler, once the Display page or F7 turns it on,
is FSR 4's Lite model (`--no-fgmod` plays `runtime/` without it). Loaded with the scene on
MSAA, OptiScaler costs nothing measurable (`deck-run.sh --fgmod`: 9.19 against 9.23 ms of GPU
time a frame in the Tokyo race in the rain, td-472 and td-470). With a HelixSR release folder
whose setup has built HLX DLSS's network (`~/helixsr`, or `PGR4_HELIXSR`), `--install` also makes
`runtime-helixsr/`, the build with HelixSR Scaler as `dxgi.dll` and `helixsr-optiscaler.ini`'s
settings, and both entries play that instead: the upscaler is HLX DLSS (`--no-helixsr` plays as
without it). The network is built from NVIDIA's DLL for that PC, so only its setup makes it.

`deck-run.sh` keeps a prefix of its own and starts each run with a fresh save. It drives a
scripted route from boot to a race (`scripts/routes/route-stpetersburg.keys` by default, played
by `scripts/route_player.py`; the other routes there drive Quebec in snow, Tokyo at night in rain,
and St Petersburg with pauses) and reports the frame rate, 1 % lows, pacing, GPU load and clock,
APU power and the busiest threads. The `primary` mode (the default) runs the native renderer,
`none` runs Xenos alone, `oracle` runs both and compares their front buffers by PSNR, and
`release` runs an unpacked release package the way a player starts it. Its header describes
every option, among them `--perf`, `--pmu`, `--server-trace`, `--draw-log`, `--catch`, `--limit`
and `--hdr`.

The run scripts refuse to start while another game is running, and stop only their own prefix's
processes.

## Debug

A debugging session (`play.sh --debug`) adds the frame CSV, the renderer's statistics and GPU
pass times every 150 frames, `deck_sampler.py`'s GPU, power, thread and memory samples, the
glitch catcher, marks, and a perf profile of every PGR4 and Geometry Wars process. Its files go
to `~/pgr4-proton/play/logs/<stamp>-*`.

- **Marks.** F12 writes the native front buffer to `captures/f<frame>-native.png` and the 5 s
  before it to `<stamp>-mark-<frame>.rec`. It also logs the next frames' draws to
  `captures/draws-f<frame>.txt` and checks them against the guest's ucode, registers and memory
  (`--pgr4_native_mark_audit=N`), with the findings in the game log. A Steam Input binding can
  send F12 from a back grip.
- **Glitch catcher.** `--pgr4_native_glitch_catch` keeps the last 5 s of shown frames in memory
  and writes the frames around each whole-image jump. `scripts/analysis/flicker.py` and
  `marks.py` turn the dumps into PNG sheets with the frame CSV's counters.
- **Renderer switch.** F9 switches to the Xenos plugin and back at the next Swap. A bug that
  stays with Xenos is in the game or the SDK, and one that goes away is the renderer's. The
  plugin draws only the game's own 1280x720 frame (`src/native/frame_height.h`), so compare in a
  run that starts on it (`--pgr4_native_classic`) or at `--pgr4_render_height=720`; at 16:10 F9
  only logs that.
- **Upscaler switch.** F7 switches the 3D scene between MSAA and FidelityFX Super Resolution at
  the next frame, and F8 (`--pgr4_native_fsr_debug`) shows FSR's debug view: its color, depth,
  motion vector and reactiveness inputs and its output, in a grid, with frame generation's view
  of the generated frames and FSR 4's watermark of the model it runs. The motion vector tile
  shows a draw whose motion is wrong (an unmatched draw is still, and ghosts). Through
  OptiScaler only the watermark follows F8, and OptiScaler's menu has the debug view. The
  Developer settings turn the keys off (`--pgr4_native_debug_keys`). The achievements overlay
  is on F6.
- **Draw logs.** `--pgr4_native_draw_log_from=<frame>` with `_draw_log_frames=<n>`, or
  `deck-run.sh --draw-log N`, logs every draw, clear, resolve and render target creation of those
  frames. Diffing a glitch frame's log against its neighbours shows what changed.
- **Debug mask.** `<capture dir>/draw-debug.txt` holds `<mask> [max_draws] [skip_from]
  [skip_to]` and is polled every few frames. Its bits turn off depth (1), culling (2), blending
  (4) or alpha test (8), log one frame's draws (16), or upload vertex data on every draw (32).
  Delete the file after use.
- **Checks.** These are slow. `--pgr4_native_coherency_check` checks vertex and index data, copy
  segments and the render thread's registers against guest memory. `--pgr4_native_track_verify`
  checks the tracker against the guest's register shadow. `--pgr4_d3d_tap` with
  `--pgr4_d3d_tap_check` checks the tracked state against the decoded push buffer. Also useful
  are `--pgr4_native_gpu_serialize`, `--pgr4_native_resolve_selftest`,
  `--pgr4_native_texture_trace=<address>` and `--pgr4_native_shader_dump_dir=<dir>`.

The `REXCVAR_DEFINE_*` lines in `src/` describe every setting.

## Measure

- **Use release flags on an idle Deck.** Turn off debug decoders, traces and draw logs, and make
  sure nothing else is running (`pgrep -l 'pgr4_recomp|deck-run|gamescope-wl'`, and check the
  screen). Background work takes power from the APU's 15 W budget, and the GPU clock pays for
  it.
- **Report CPU and GPU time.** On the Deck the CPU takes most of the shared power budget, so CPU
  time saved on any thread turns into GPU clock, while GPU work saved shortens the frame
  directly. The frame CSV has per-stage CPU times (`--pgr4_native_profile`), and
  `--pgr4_native_gpu_timing` logs GPU time per pass.
- **Compare runs statistically.** Runs of one build differ from each other. `ab_compare.py`
  compares two builds in matched bins of the same route with a 95 % interval. A change is
  accepted when B is faster with the interval excluding zero, the 1 % low does not drop, and the
  oracle's PSNR stays within about 1 dB of its baseline on each capture.
- **Measure the same window.** `frame_times.py` reports a fixed race window, the same span of
  game frames in every run, and checks the route against its reference draw counts
  (`scripts/routes/*.draws`). A run whose car took another line reports where it diverged.

| Tool (`scripts/analysis/`) | Purpose |
|---|---|
| `perf_fps.py`, `frame_times.py` | Frame rate over a window, and the distribution, 1 % and 0.1 % lows, hitches and route check |
| `ab_compare.py` | Two runs compared in matched bins with a 95 % interval |
| `pacing.py` | Time steps, shown intervals, start-to-display latency and their mismatch |
| `perf_stats.py`, `perf_race.py`, `perf_hitch.py` | Stage costs per draw, and slow frames with their causes |
| `perf_symbols.py` | Perf samples per thread, symbolized with the runtime's `.map` files and Wine's exports (`--window`, `--annotate`) |
| `oracle_diff.py`, `compare_resolves.py` | Native against Xenos captures per tile and band, and resolve outputs against the plugin's |
| `flicker.py`, `marks.py` | Flashes and whole-image jumps in glitch catcher dumps, and the seconds before each mark |
| `rt_passes.py`, `trace_summary.py` | Render target passes in a frame trace, and frame trace summaries and checks |
| `hdr_shot.py` | Luminance distribution of HDR10 AVIF screenshots |
| `shots.py` | Change, sharpness and pixels of given colors per `--shots` screenshot, and a contact sheet |
| `scan_d3d_surface.py` | Generates `src/d3d_surface.inc` from the generated code |
| `tu_address_map.py` | Matches functions between two executables instruction by instruction |
| `lua51_dis.py` | Extracts the game's compiled UI scripts from its image and lists them as `luac -l -l` would |

`deck-run.sh` also runs the samplers in `scripts/`. `deck_sampler.py` records GPU load and
clock, the APU's power split, busy threads and other processes. `gpu_clock_sampler.py` samples
the APU every few milliseconds. `pmu_summary.py` reports instructions per cycle, fetch stalls and
instruction cache refills per thread, and `server_requests.py` counts wineserver requests per
thread.

## SDK patches

The SDK is ReXGlue's `development` branch at the commit `patches/rexglue.lock` names, with the
patches in `patches/` applied in order. `scripts/build.sh` re-patches its checkout whenever the
lock or a patch changes, and CI checks that the series applies.

- Take a fix from ReXGlue `development` or Xenia Canary where one exists, and name the source in
  the patch message. Ported code keeps its BSD-3-Clause notice (`THIRD_PARTY_NOTICES.md`).
- Game-specific behaviour belongs in `games/<title>/`, as mid-asm hooks in the manifest or
  settings in the TOML, not in the SDK.
- To change the series, commit in the SDK checkout (`<dest>/rexglue-sdk`) on top of the applied
  patches, then `git format-patch` the series back into `patches/`. Each patch holds one change,
  with a message that says what it changes and why.
- To move to a newer ReXGlue, change the commit in `patches/rexglue.lock`, drop the patches
  upstream already has, rebase the rest and rebuild with `--force`.

The renderer uses these ReXGlue integration points.

- `REX_HOOK_RAW`, where an app's strong `sub_X` overrides the generated weak alias and
  `__imp__sub_X` reaches the original.
- `[[entrypoint.midasm_hook]]` entries in the codegen manifest.
- The shader translator sources, which `src/native/shader_xlate.cmake` compiles into the app
  because the plugin keeps them hidden.
- The key binds, and `--gpu_skip_draws` on the plugin's command processor.

## Performance rules from other ports

Open-source Xbox 360 recompilations with renderers of their own have solved most of the problems
this one meets. Read their code first, adopt the technique, then confirm it with one measured
run. The main sources are `hedge-dev/UnleashedRecomp` and `sonicnext-dev/MarathonRecomp` (their
own D3D12 and Vulkan renderer, `gpu/video.cpp`), `shipa-2/Xerenge` (a PM4-level renderer on
ReXGlue), `LittleBitUA/DPRecomp`, `SpeedBreakerProject/speedbreaker`,
`fungalspores/prototype-switch`, Xenia Canary, and DXVK and vkd3d-proton.

1. **Replace the guest D3D API rather than wrap it.** UnleashedRecomp replaces its draw and state
   functions. A wrapper that calls the original keeps paying for packets nothing draws. Keep the
   originals only for what the platform still needs, such as ring kicks, fences and Swap.
2. **Use a render thread fed by a queue of small commands.** The render thread owns every
   graphics API call. Send deltas, not per-draw state snapshots.
3. **Translate shaders offline and compile pipelines before they are needed.** Compile a
   recorded pipeline list in the background, and compile misses asynchronously.
4. **Bind textures and samplers bindlessly**, with their indices in a small constant block.
   Upload constants as whole blocks from a linear per-frame allocator, only when dirty, and bind
   them as root descriptors.
5. **Defer resolve copies** where the surface can be bound directly, and keep uploads out of
   render passes, since an upload inside a pass ends it.
6. **Reset the cached depth bias after `SetPipelineState`.** A pipeline change reloads the depth
   bias even with `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS`.
7. **Pace on the display.** Feed the game's time step from the intervals the display shows, since
   a timer of its own drifts against the display. Check pacing over minutes, not seconds.
8. **Use register-as-local codegen** (`cr_as_local`, `ctr_as_local`, `xer_as_local`,
   `non_volatile_as_local`, `reserved_as_local`). It speeds up all guest code, which sets the
   floor no renderer change can lower. The manifest explains why `skip_lr`, `skip_msr` and
   `non_argument_as_local` stay off for this game. Text patches against generated code break
   when these options rename registers, so use mid-asm hooks instead.
9. **Count wineserver requests under Proton.** Calls that Windows handles in its kernel (waitable
   timers, thread priority queries, resuming running threads, HID) become requests to a separate
   process there, and each costs context switches and wakeups paid from the power budget.
10. **Keep hot threads cheap.** Never read write-combined memory, avoid 256-bit vectors on the
    Deck, and read the TSC rather than `QueryPerformanceCounter`, which is a syscall under Wine.

## Light flares

The headlights, taillights and street lights dim as the traffic hides them, each from an
occlusion query of its own. The game renders the scene with 2x MSAA, resolves its depth into
guest textures, redraws that depth at 1x into a second buffer (on the Xenos the step is an
exact EDRAM copy; here the game runs a depth-writing pixel shader for it), and then draws each
flare additively against it with GREATER_EQUAL inside one query. The flare's brightness is
1 - ZPass / Total, and the depth it tests against is the previous frame's.

Why the flares flickered, measured with `--pgr4_native_occlusion_trace` and
`scripts/analysis/occ_flicker.py`:

- The raw test flips fully occluded to fully visible and back about once per 50 frames per
  light, on the GPU side (the retire stream), not in the delivery. The flips are geometric:
  the live sprite is compared pixel for pixel with the previous frame's depth of a moving
  scene, so as the car under a flare moves, the stored depth at the sprite's pixels wobbles by
  the panel's relief, more than any depth margin.
- No bias fixes that. The guest's polygon offsets are absolute z (the float24 factors of the
  SDK's draw.h), but on the host's float32 depth the rasterizer bias scales with the
  primitive's exponent, so the flares' ~3e-4 margin vanishes near z=1; a test-side bias of
  even 1e6 changed nothing, and subtracting 3e-4 from the resolved depth moved the visible
  share 50 to 72 % but cut the flips by only a tenth. A faithful fix would be 20e4 depth end
  to end (the translator's DSV_IsWritingFloat24Depth already writes it), not a bias.
- The delivery is a windowed sum instead (`--pgr4_native_occlusion_window_ms`, 100): the
  delivered ZPass and Total are the sums of the query's frames recorded within 100 ms of its
  newest, so the game's own fade averages them. A blink becomes a shallow dip, and a real
  occlusion still wins within the window. With four frames the flips fell from 15.8 to 7.2 per
  1000 reads, and the remaining ones are a ~3 % brightness wobble of a light already at the
  edge of visibility, not a blink.
- A light's test covers a few pixels (Total 2 samples at the 25th percentile of a Tokyo night
  race, 4 at the median, 10 at the 75th), so one frame's result is nearly all or nothing even
  when the light goes behind a car's edge. The window is a time rather than a frame count so
  that such an occlusion fades over the same 100 ms at every frame rate (9 frames at 90 fps, 3
  at 30), where the original's single 30 fps frame was a blink. More samples would not help:
  the depth the sprite tests against has no more detail than its pixels.

## Native 16:10

The game renders at 1280x720, and the renderer shows it letterboxed on the Deck's 1280x800 panel,
never stretched. Rendering at 1280x800 needs a game patch rather than a renderer setting.

- Device creation (`sub_82317D48`) sets the presentation size after `XGetVideoMode` (0x82317D80,
  0x82317D98) and sizes the front buffer (0x82317E6C, 0x82317E74). Changing only those moves the
  front buffer but leaves the frame as it was, because the scene, its resolves and the
  post-processing chain keep their own sizes.
- The scene renders at 1280x720 with 2x MSAA in two predicated tiles (rows 0 to 480 and 480 to
  720), and 800 rows fit the same two tiles. Post-processing runs at 1/4 to 1/32 size, each pass
  a resolve into a guest texture of that size, followed by the tone map into the front buffer.
- A patch has to change every render target, viewport, scissor, clear and resolve rectangle of
  the scene and the post chain to the new height, and allocate the guest textures they resolve
  into to match. It also has to adjust the projection, either keeping the horizontal field of
  view and adding height or cutting the sides. The HUD and menus need anchoring to the edges, as
  UnleashedRecomp's `aspect_ratio_patches.cpp` does, and photo mode, replays and the garage may
  create their own targets.

That patch shipped (`pgr4_render_height`, default 800): the frame renders at 1280x800 end to
end at the same 90 fps, the scene keeps the game's horizontal field of view and gains vertical,
and the HUD and menus keep their shape (square pixels, centered). The mechanism, all in `pgr4recompiled_manifest.toml` + `Pgr4*Hook` in
`games/pgr4/src/pgr4_hooks.cpp`:

- Every instruction that loads the hardcoded 720 of the frame (surfaces, the mode structs, the
  video-mode table's selected case) gets a mid-asm hook that rewrites the register to the render
  height. The post chain needs nothing: it derives 320x200, 160x100 and down from its sources.
- The sizes that arrive as structs instead of immediates are patched at the D3D boundary:
  `SetViewport` (the int one, `sub_826C6378`), the float viewport setter the scene's views use
  (`sub_826C63F8`, `{X, Y, W, H, MinZ, MaxZ}` floats), `SetScissorRect` (`fn_826C59D8`) and
  `Resolve` (`sub_826CBEE0`) take a 1280x720 struct to the render height.
- The scene's render targets come from the pair creators `fn_826C8B18`/`sub_822B3FC8`; their
  height argument is hooked too, because `sub_826C6100` clamps every viewport to the bound
  target's packed size and would otherwise clamp the scene back to 720.
- The HUD and the menus share one orthographic projection, vertex constant c45: 640x480 mapped
  into the central 960x720 of the frame (4:3-safe, the pixel scale 1.5). Its y scale and offset
  are the one 720 the loads cannot reach (the game computes them). The game writes c45 through
  SetVertexShaderConstantF (`sub_826C9908`) and also with stores of its own (the text draws,
  `sub_824D0840`), so the renderer replaces the game's value where every draw flushes it
  (`GuestStateTracker::FrameHudProjection`, `src/native/d3d_state.cpp`): the central 960x720
  of the taller frame, centered, square pixels. The UI's space is then 640 by 480 x height / 720
  rows (533 at 800), its 480 rows in the middle.
- The UI's renderers clip with SetScissorRect, from rows-to-pixels factors of their own: the 2D
  items' (`sub_824D0840`) and the 3D models' (`sub_826400B8`) scale rows by the frame's height /
  480 with no offset, which at 800 rows puts a clip up to 40 pixels off the projection's rows,
  the most at the screen's edges. `Pgr4UiClipHook` and `Pgr4UiModelClipHook` give them the
  720-row frame's factors moved down by the rows added above, and `Pgr4UiClipRectHook` takes a
  clip that reaches the game's screen edge (row 0 or 480, as the HUD's items and the race
  intro's ticker have) to the frame's edge.
- Every view's perspective is built in `sub_822C2410` (the call of `sub_826E32F0`: vertical
  field of view, aspect) with the 16:9 frame's aspect, the scene's through its pixel aspect
  (view + 440) and the UI's 3D models' (the arcade's medals) as the view's width / height. A view
  of the frame's shape gets the frame's aspect and the vertical field of view widened by height /
  720 (`Pgr4ViewProjectionHook`), so the horizontal one stays the game's; v0.3.0 stretched the
  scene 11 % taller and narrowed the medals instead.
- The UI scripts keep the game's screen (`games/pgr4/ui/patches/`). The menus and loading
  screens lay out in rows 0 to 480 as the game composed them, so their items stay with their
  frames and backgrounds; the 3D backgrounds of the menus fill the frame, and their 2D items sit
  in its central 720 rows. A version that made the screen as tall as the frame (v0.3.0, v0.3.1)
  sent what the menus anchor to the screen's edges to the frame's, away from the frames and art
  placed from the middle, and needed a fix per screen. What belongs to the frame rather than to a
  screen's layout uses `UIGlobals.frame` (`UIGlobals.fnl`): `HudHelper.fnl` moves each HUD item
  above its viewport's middle up with the frame's top, and each below it down with the bottom;
  the widgets that place themselves in the game's code (the rev counter: `sub_8245C2A8`,
  `sub_8245C310`) move the same way through `Pgr4HudWidgetHook` and `Pgr4HudWidgetBHook`.
  `GUIBank.fnl` moves the music player down with the bottom and has the previous screen's image,
  which a screen fades from, cover the frame. `UIGlobals.fnl` has every wash, a graphic of one
  colour over the whole screen (the fades to a colour, the backdrop that dims a race behind the
  pause menu and its popups), cover the frame; textured art keeps its shape. The UI's 3D models
  (the arcade's medals) need nothing: their view keeps the 720-row frame's rows about the middle
  (`Pgr4ViewProjectionHook`), as the 2D items do.
- FSR's sizes (`pgr4_native_upscaler_height`, `_min_height`) are rows of the scene, which the
  settings keep as a fraction of the next start's height: a change of aspect ratio rewrites them
  for it, and the renderer clamps them to the running scene until the restart.- `scripts/analysis/pgr4_resolution_sites.py` maps every 720/1280 load in the image to its
  function; the manifest's comments name the false positives it deliberately skips (VMX byte
  offsets, allocation sizes, string calls, the 1280 widths).

## In-game settings

The port's settings are three pages, Display, Driving and Developer, in the game's Options
menu and in the pause menu of a race, drawn by the game's own UI code. The UI is Lua 5.1: 247
compiled scripts in a `BZUI` archive in the image, which `scripts/analysis/lua51_dis.py`
extracts and lists.
`games/pgr4/src/pgr4_lua.cpp` replaces the archive's chunk loader (`sub_82508360`), so the game
also loads the scripts in `games/pgr4/ui/`: a screen of the port's own, and patches that run
after the game's script of the same name (the menus' entries). They are written in Fennel,
which the build compiles to Lua with the compiler `scripts/fennel.lock` pins, against the
globals listed in `games/pgr4/CMakeLists.txt`, so a misspelt name fails the build. `pgr4_settings.cpp` holds the
options, the cvars each choice sets, and the `Pgr4` table the scripts call.

- **Text.** The game's text functions take a string ID, which its string table
  (`sub_8289B1D0`) looks up. `Pgr4.Text` gives the port's strings IDs from 0x8000, and the
  lookup answers them.
- **The UI's habits.** It marks the first row only after a screen's `Init`, so the screen
  activates its rows at its first frame. `SetActive(false)` hides a spinner's backing but not
  its label, so hidden rows move below the clipped frame. A spinner lists its items last added
  first, so the screen adds them in reverse.
- **Applying.** A setting the renderer can change while it runs is set at once, through
  `NativeRenderer`'s `Request*` taken at the next frame; the others are marked `*` until the
  next start. Both are saved to `userdata/pgr4_settings.toml`, read after
  `pgr4_recompiled.toml`. A flag on the command line holds for its session only: the file keeps
  its own value for that cvar until the menu changes the option. Leaving a page with a `*`
  choice changed shows the game's popup (`SetupCustomPopup`) saying a restart applies it.
- **Testing.** `scripts/routes/route-settings.keys` opens the pages on a fresh profile, changes
  a start-up and a live setting on Display and both of Driving's, saves and reopens.
  `route-pause-settings.keys` opens them from the pause menu of a race.
  `--pgr4_game_debug_menus` shows the game's own debug entries in its menus.

## Gearbox

What the game does with the shift buttons and where its automatic decides. Offsets are from the
base of each object. All of it was read from Title Update #2's generated code. Probes on a
St Petersburg race confirmed the pad, the bindings, the controls and the automatic's numbers,
and that `sub_822F9D70` and `sub_822FB680` never run for the player's car.

- **Pad.** The state of player N is at `0x82A90694 + 1888 * N`, refreshed once per frame by
  `sub_82310F60` through `XInputGetState`. Held buttons are at +1692 (bits 0 to 15 are the
  raw XInput buttons, bits 16 to 25 trigger and stick thresholds), the previous update's at
  +1696, and this update's presses at +1700. Each action has two bindings, button indices at
  +492 + 8a and +496 + 8a (26 means none). `sub_8230FC50` writes the menu bindings, and a race
  installs the driving layout from the manual. In that layout action 12 is shift up (Y), 13 is
  shift down (X), 10 is the handbrake (A) and 18 is the taunt (B). `sub_822DC9E0` reads an
  action's press, `sub_822DC978` its hold and `sub_822DC910` its analog value.
- **Controls.** The player's driver (`sub_822E2880`, called through its vtable) fills the
  controls at [driver + 808] in `sub_822DC420`. Throttle is at +4, brake at +8, steering at +12,
  the shift up and shift down presses at +16 and +17, and the handbrake at +18.
  `sub_822F9D70` and its gearbox `sub_822FB680` are similar code that the player's car does not
  use.
- **Update.** `sub_822DE628` updates the gearbox `gb` of the car at [gb + 36] once per
  simulation step. A car whose +20 is 2 is driven to the gear at car + 2804. For every other
  car the step runs the automatic, `sub_822DE250`, and then, once per update (the latch at
  gb + 29), shifts up on controls +16 or down on +17. The current gear is the signed byte at
  gb + 30 (-1 is reverse, 0 is neutral), and the time left in a shift is at gb + 16.
- **Automatic.** `sub_822DE250` decides only when car + 61 is set (the automatic transmission)
  or car + 20 is 1. The player's car has +20 at 0 and +61 set; the opponents have +20 at 1.
  It shifts up once gb + 16 has fallen below a constant, the gear is below the top gear (car
  data at [car + 804] + 412), the engine speed at gb + 0 (rad/s) exceeds the upshift point at
  car data + 408 (rpm, 6500 to 7900), and gb + 31 is set. It shifts down while the next lower
  gear, estimated from the wheel speeds, the gear ratios (car data + 472), the final drive
  (+468) and the wheel radius (+416), would run under car data + 440 (0.82 to 0.9) times the
  upshift point: `f31` after `fmuls f31,f10,f12` at `0x822DE400`. It also handles reverse when
  stopped.
- **Shifts.** `sub_822DDBE8` shifts up (from neutral to 1st, never past the top gear) and
  `sub_822DE150` shifts down (to 1st at walking speed, and on to reverse). Both set gb + 28 and
  extend gb + 16 to the car's shift time, car data + 436.

With a factor of 0.82 and about 1.4 between gears, the engine falls to under 60 % of the
upshift point before the automatic takes the lower gear. `Pgr4AutomaticDownshiftHook` (after
`0x822DE400`, behind `--pgr4_automatic_keep_revs`) raises the factor to 0.9 for the player's
car. A tenth of the speed between the two shifts keeps a downshift from shifting straight back
up. A different automatic altogether would be a mid-asm hook at `0x822DE250` with
`return_on_true`, which skips the game's automatic. Its decision would be a press of controls
+16 or +17, so the game's own `sub_822DDBE8` and `sub_822DE150` shift with their clutch timing
in the same step. To test the manual without the menus, give a fresh profile the manual
transmission by setting `XPROFILE_GAMER_RACE_TRANSMISSION` (0x10040026) to 1 in the SDK's
`user_profile.cpp`.

## Vehicle lights

Whether a car has its lights on is the race lighting's, not the car's. Offsets and functions are
Title Update #2's.

- **Lighting.** A race lighting's settings carry `UseFogLights`, `UseCarNightLights` and
  `UseBikeNightLights`, bytes at +210, +211 and +212 (their reflection descriptors are at
  0x82A2BEC8 to 0x82A2BEF0). `sub_823CDA80` blends two lightings and takes the flags from the
  nearer one, and `sub_823CF460` copies a lighting into the scene's, 0x82A1D220, at +52, +53
  and +54. The copy runs about 1350 times a second, always on that lighting. In the St
  Petersburg day race the car flag is 0 and the bike flag 1.
- **Readers.** `sub_82415138` gives each vehicle its lights from +53, or +54 for a bike
  (vehicle + 1712 is not 0), and `sub_82421E68` and `sub_82448990` choose the lit or unlit lamp
  parts by +53.
- **Override.** `Pgr4SceneLightsHook`, after the copy's `stb r11,54(r31)` at `0x823CF71C`, sets
  both flags from `--pgr4_headlights` (race, on or off), the Driving page's Vehicle lights.
  `bind_headlights` (F5) sets them to the opposite of what +53 holds and keeps that choice until
  the setting changes. The hook reads the choice from an atomic, not the cvar's string, which the
  menu's thread writes.
  `scripts/routes/route-stpetersburg-headlights.keys` switches them on the grid.
- **Debug variables.** `Vehicle.Lights.OverrideNightLights0` to `7` and `Car.Lights.Enable` are
  registered (36-byte records at 0x82AA7988 and 0x82AB5648, the value at +32), but no game code
  reads them in the shipped build.

## Releases

Pushing a `v*` tag builds on GitHub Actions (`.github/workflows/release.yml`, with the game files
from a private repository) and publishes a ZIP made by `scripts/release/package_release.py`. The
ZIP holds the executables, the ReXGlue DLLs, the settings from `games/*/`, the player README
(`scripts/release/README.md`) and the licenses, taken from an explicit file list. The packager
refuses an instrumented build. `deck-run.sh --mode release` runs an unpacked package the way a
player starts it.

## Rules

- Keep game data out of git. That means discs, executables, title updates, extracted assets,
  saves, captures of game imagery, and C++ generated from the game's executables. CI checks the
  common extensions.
- Never use leaked or copyrighted source code. Code from projects without a license can serve as
  a reference but is never copied.
- A build that compiles is not evidence. Run the game, and keep the screenshot, frame-time log,
  `ab_compare.py` verdict or oracle PSNR that shows the result.
- Keep logging off production paths, and make debug output opt-in through a setting.
