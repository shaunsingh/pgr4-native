# Architecture

This document describes how the native renderer works. It covers where the renderer sits in the
recompiled game, its threads, the flow of a frame, the rules that keep its copies of guest data
correct, and its modules, caches and memory.

## Where it sits

PGR4 is recompiled with ReXGlue from the game projects in `games/`, on the ReXGlue commit that
`patches/rexglue.lock` names with `patches/` applied. In a plain ReXGlue title the guest's Xbox
360 D3D library writes PM4 packets into a ring buffer, and the Xenos plugin (`rexgpu-xenos.dll`,
Xenia's D3D12 backend) replays them on its `GPU Commands` thread, emulating the Xbox 360 GPU.

This renderer works one level up. `src/pgr4_d3d_trace.cpp` hooks the guest D3D library's own
functions in the recompiled code with `REX_HOOK_RAW`, at the addresses in `src/d3d_surface.inc`
(generated from the generated code by `scripts/analysis/scan_d3d_surface.py`). Each hook calls
the original guest function, so the guest device and its register shadow stay exactly as the
game expects. For the outermost draw, clear, resolve and Swap, the hook then passes the call to
`NativeRenderer`, which reads the Xenos register state the guest D3D left in its device and
records D3D12 work.

The renderer hooks the D3D API rather than sitting behind ReXGlue's command processor as a PM4
plugin, for three reasons.

- Every game draw goes through the D3D draw functions, and the D3D device keeps the GPU
  registers verbatim at fixed offsets, with dirty masks. The hooks see the complete state of
  each draw without parsing packets.
- The game renders with predicated tiling, so the D3D replays each frame's draws once per EDRAM
  tile plus an untiled pass. At the API each draw arrives once, before tiling, and the renderer
  draws it once at full size. A PM4 renderer would have to parse every pass and drop the
  replays.
- The Xenos plugin stays in the same binary for comparison. Both can render the same frame, and
  one key switches between them.

The cost is that the hooks depend on this executable's addresses, so a different build of the
game needs a rescan. State that the D3D writes into the push buffer outside its shadow also has
to be read by hand, in `ReadDrawState`.

In primary mode the guest D3D's packet building for draws is wasted work, so `guest_d3d.cpp`
replaces `DrawIndexedVertices` outright (`--pgr4_native_replace_draws`). The replacement leaves
the device in the state the original leaves it in and skips the packets. Ring kicks, fences,
Swap, Clear and Resolve stay on the original path. UnleashedRecomp and MarathonRecomp replace
their guest D3D in the same way.

## Modes

With `--pgr4_native`, the native renderer runs beside the Xenos plugin and presents into a debug
window. Both render every frame, and captures compare the two front buffers by PSNR.

With `--pgr4_native_primary` (which implies `--pgr4_native`), the native renderer presents into
the game window. It sets `--gpu_skip_draws` (`patches/0031`), so the Xenos command processor
still parses the stream and runs fences and interrupts, but skips draws, copies, clears, shader
loads and swaps. The renderer counts the game's occlusion queries itself (`occlusion`), and
`occlusion_query_external` (`patches/0049`) keeps the command processor from writing fake
counts into the query reports.

The swapchain belongs to the game's top-level window, and `NativeRenderer::OwnGameWindow`
disconnects ReXGlue's presenter from that window before creating it. Gamescope flips only a
top-level window's surface. A child window's frames would go through Xwayland's copy, which
ignores FIFO and frame limits.

F9 (`bind_renderer_switch`, `--pgr4_native_renderer_switch`) hands the game to the Xenos plugin
at the next Swap, and back. Handing over marks the guest D3D's whole state dirty so that the
next draw writes all of it into the stream, clears `gpu_skip_draws`, releases the swapchain and
reconnects ReXGlue's presenter. `NativeRenderer::Live()` then returns null and the hooks only
forward. Taking the game back resyncs the state tracker from the device. The same hand-over
happens while a XAM dialog is up, since ReXGlue's presenter draws those, and when a frame fails
(`Fail`). Neither renderer has the other's render targets, so the first frame after a switch can
be black, and the game's eye adaptation, kept in one of them, settles again over the next second.

## Threads

| Thread | Owner | Work |
|---|---|---|
| `Main XThread` (guest) | the game | Runs the game. The hooks run here and track state, then capture each call (or run the whole native path with `--pgr4_native_render_thread=false`) |
| `PGR4 native render` | `NativeRenderer` | Replays captured commands into the modules, and owns the command lists, the D3D12 objects and all module state |
| `PGR4 native untile` (1 to 4) | `TextureCache` | Arms a texture's page watches, untiles it into upload memory, and records the uploads of textures first bound in the frame on an upload list of its own |
| `PGR4 native watches` | `DrawRenderer` | Arms the page watches of copied vertex and index runs, and keeps each page valid while its hash still matches its copy |
| `PGR4 native pipelines` | `DrawRenderer` | Creates pipeline states, loads and stores them in the pipeline library, and prewarms the stored list |
| `PGR4 native pacer` | `FramePacer` | Waits on the frame fence and the swapchain's frame latency waitable, then opens the gate for the next guest frame and sets its delay |
| file writer | `AsyncFile` | Writes the frame CSV and the pacer log out of memory, away from the render and pacer threads |
| `GPU Commands` (SDK) | Xenos plugin | Parses PM4, and in primary mode runs only fences and interrupts |

## A frame

1. **Tracking.** On every state-flushing D3D call, the game thread reads the dirty masks before
   the original runs. `GuestStateTracker::AfterCall` then copies the register blocks the call
   flushed from the device shadow into a 0x5000-register file (float constants at 0x4000 and
   0x4400, fetch constants at 0x4800, bool and loop constants at 0x4900, render state from
   0x2000). It also copies the window scissor and, for draws, the bound shaders. Groups are
   compared 128 bits at a time, and each changed value is appended to the render queue's batch.
2. **Capture.** For the outermost draw, clear, resolve or Swap, `OnHook` captures what the
   render thread will need. A top-level `BeginVertices` is held until the next hooked call, by
   which time the game has written its vertices.
   - A draw runs `ReadDrawState` (primitive, counts, index buffer, inline vertices, shaders) and
     `DrawRenderer::Prepare`. Prepare looks up the shaders, reading an immediate vertex shader's
     patched copy while it is still in the push buffer. It copies the vertex and index data into
     upload memory (`RequestRange`) and converts triangle fans to indexed lists.
   - A clear runs `RenderTargets::DecodeClear`, a resolve runs `DecodeResolve` (whose arguments
     are on the guest stack), and a Swap runs `Presenter::ReadSwap` for the front buffer's fetch
     constant.
   - Commands go to the render thread in batches of 256, and a Swap ends a batch.
3. **Recording.** The render thread applies each command's register changes to its own register
   file, then runs the module (`Draw`, `Clear`, `Resolve` or Swap). Every
   `--pgr4_native_submit_draws` draws it submits the frame so far, once the untile workers have
   filled the upload memory that its copies read. At Swap it records the present blit, waits
   for the untile workers (`TextureCache::FinishUploads`), and submits their upload lists ahead
   of the frame's list in one `ExecuteCommandLists`. `EndFrame` then closes, executes, presents
   and signals the fence, and the frame's upload memory is retired and its CSV row written.
4. **Pacing.** The game thread blocks at Swap while the render thread is more than one frame
   behind. In primary mode the guest presents immediately (`Pgr4PresentationInterval`, a mid-asm
   hook in `games/pgr4`), and `FramePacer` (`frame_pacer.h`) is the only clock. Frame F + 1
   begins once the display has shown frame F + 1 - L, where L is `--pgr4_native_frame_latency`,
   counted on the swapchain's frame latency waitable. It then waits a delay so that it finishes
   just before its slot (`--pgr4_native_low_latency`, DXVK's latency sleep measured against the
   display). The game's time steps follow the intervals the display shows, at any refresh rate
   or frame limit. The motion blur pass scales its per-sample step by the frame's time step,
   smoothed over ~1/2 s (`Pgr4MotionBlurStep`), so the blur is as long as at the console's
   30 FPS and steady on a still frame: with the raw step, its jitter was the whole of the
   visible blur in pause, and the still frame pumped (issue #1).

## Coherency rules

The game reuses memory as soon as its fences pass, and in primary mode they pass almost at once.
Anything the render thread reads late must not change under it.

- Vertex and index data are copied at capture time, on the game thread (`RequestRange`).
- Texture data are read late, on the untile workers, before the frame's list is submitted. Each
  job arms the page watch just before it reads, so a later write marks the texture stale again.
  Every draw is stamped when it is captured, and a draw captured before a newer write to a
  texture binds the texture as it was (`--pgr4_native_texture_capture_order`, CSV `tex_stale`).
- Shader ucode is read at capture time. Immediate vertex shader loads are patched copies in the
  push buffer, and the D3D can patch a bound variant's vertex fetches in place. The shader
  reference carries the variant's binding record, so a rebind misses the memo.
- Resolve, clear and Swap arguments are read at capture time, since they live on the guest
  stack.

## Modules (`src/native/`)

| File | Contents |
|---|---|
| `renderer`, `frame_height.h` | The run's frame height (`FrameHeight`), `NativeRenderer` with start-up, settings, the `On*` entry points, the render thread and its queue, the frame CSV, the log line, the renderer switch and `Fail` |
| `hook_args.h` | The hooked guest D3D functions and their PPC arguments |
| `guest_d3d` | Guest D3D functions replaced in primary mode |
| `d3d_state` | `GuestStateTracker` (register file and change log), `ReadDrawState` and the device layout |
| `d3d12_device` | Queue, fence, frames in flight, back buffers, the debug window or the game window's swapchain, HDR10 colour space and metadata, `EndFrame`, and barrier and buffer helpers |
| `draw` | `DrawRenderer` with shader selection, the pipeline key and cache, bindless textures and samplers, system, float, bool and fetch constants, vertex and index upload, fixed-function state and the draw itself, plus `Prepare` for the capture step |
| `shader_cache`, `shader_translate`, `shader_xlate.cmake` | Ucode to DXBC, keyed by XXH3 of the ucode and translated per modification with ReXGlue's DXBC translator |
| `shader_specialize` | Bool-constant specialization, which rewrites the ucode's control flow for a draw's bool constants and loads the result as a shader of its own |
| `geometry_shader` | Generated geometry shaders for points, rectangle lists and quads |
| `render_targets` | Host render targets for the guest's EDRAM surfaces, with deferred and merged clears. The scene's can have other sample counts than the guest's (`OverrideScene`: one for the upscaler, `--pgr4_native_msaa`'s otherwise); a resolve out of one averages all its samples or takes sample 0 |
| `resolve` | Guest resolves into host images keyed by destination, through compute shaders specialized for each kind of resolve. `resolve_debug` holds dumps and the self-test |
| `textures`, `texture_layout` | `TextureCache`, which maps fetch constants to host textures and handles untiling, format conversion, page watches, resolved-image copies, placed heaps and eviction |
| `upload_pool.h` | Persistently mapped upload blocks, allocated linearly and recycled by fence |
| `pipeline_store` | The stored pipeline list and prewarm. Each open counts a session, rewrites the file with what the last 8 sessions used (most recent first), and prewarms only what the last 3 did, so a start pays for what the player plays, not for everything they ever played |
| `present` | Front buffer to window (aspect fit, the display controller's gamma ramp, HDR10 output) and captures. `present_debug` holds the PNG writer, the glitch catcher and the luma probe |
| `frame_pacer` | Frame pacing on the display |
| `occlusion` | The game's occlusion queries (the light flares): host queries around each draw inside one, ZPass and pre-test coverage, delivered as the sums of each query's frames of the last 100 ms (`--pgr4_native_occlusion_window_ms`; development.md's "Light flares") and written into the guest's reports at GetData |
| `upscaler`, `motion_history.h`, `dynamic_resolution.h` | The scene antialiased by FidelityFX Super Resolution instead of MSAA (`--pgr4_native_upscaler`, switched with F7, its debug view on F8): single-sampled scene targets at the render size (`--pgr4_native_upscaler_width`/`_height`, `RenderScale`; with `--pgr4_native_upscaler_target_fps` a smaller size each frame from the GPU's time on the frames before, `D3D12Device::gpu_busy_ns`), jitter, a texture LOD bias, motion vectors from the vertex shaders run again with each draw's constants of the previous frame, and the upscaling at the scene's first resolve |
| `fidelityfx` | The FidelityFX API, loaded from `amd_fidelityfx_loader_dx12.dll` (or what OptiScaler puts there), with the scene size and camera constants the upscaler and frame generation share |
| `frame_generation` | A frame FidelityFX Frame Generation makes between each two (`--pgr4_native_frame_generation`), from the upscaler's depth and motion: the frame drawn into an image of its own, the generated frame and then it presented FIFO back to back, the pacer counting a frame shown with its second present, and a HUD-less copy of the front buffer (`Resolver::ResolveCopy` before the HUD's first draw) so the HUD is not interpolated |
| `gpu_timer` | GPU time per pass (`--pgr4_native_gpu_timing`) |
| `profile.h`, `clock` | TSC stage timers and counters (`--pgr4_native_profile`), and the CPU clock |
| `async_file`, `log.h`, `win_handle.h`, `guest_memory.h` | Buffered file writer, bounded warnings, owned Win32 handles, and guest address translation with big-endian loads |
| `pgo_counters.cpp` | Periodic profile counter dump for `--pgo gen` builds |
| `sdk_cvar_shim.h` | Keeps the shader translator's cvars from registering a second time beside the plugin's |

Files that port Xenia or ReXGlue code (BSD-3-Clause) name the ported functions in their
headers. Among them are `draw`, `render_targets`, `resolve`, `geometry_shader` and the texture
code.

## Caches and what invalidates them

Each cache lives on the thread that runs its module unless noted.

| Cache | Key | Valid while |
|---|---|---|
| Shader memo (game thread) | Ucode address, size, immediate sequence and binding record, in 256 entries that check the first and last 8 dwords | The ucode there is unchanged |
| Modification memo | Shader pair and the registers the selection reads | The inputs are equal |
| Translations | Shader and modification | Always (never freed) |
| Ready-pipeline cache | Pipeline key hash | The pipeline is ready |
| Pipelines | XXH3 of `PipelineKey` | Always (compiling, ready or failed) |
| `TextureCache::Get` memo | The 6 fetch dwords | The texture is not dirty, has no pending resolve, and its slot generation is equal |
| Bind memo | Shader and the fetch constants it reads | `change_epoch` is equal, or every bound texture is unchanged |
| SRV and sampler memos | Resource and view, or the exact sampler key | The keys are equal |
| Constant buffers (b0 to b4) | The current bytes, kept in CPU memory | The bytes are equal |
| Valid-page bitmap | 4 KB guest pages in the shared-memory buffer | No guest write hit the page. A copied run stays pending, and is hashed by each request, until the watch thread arms it |
| Resolve targets | Destination addresses | No resolve since. A resolve marks the textures there pending and bumps `change_epoch` |
| Pipeline library | Root signature hash, description and bytecode | The driver accepts the library, which is otherwise rebuilt |
| Stored pipeline list | Key, with shader pointers replaced by ucode hash and modification | The header matches `sizeof(PipelineKey)` and `kPipelineKeyVersion` |

`change_epoch` (`textures.h`) goes up on every guest write that marks a texture dirty, every
resolve into a texture, every upload and every creation. The fault handler increments it after
setting the dirty flag (acq_rel), so a memo that read the old epoch rechecks and sees the flag.

Bump `kPipelineKeyVersion` in `draw.cpp` whenever a `PipelineKey` field changes meaning, or old
stored lists will prewarm the wrong keys.

## Memory

| Pool | Notes |
|---|---|
| Shared memory buffer, 512 MB | Mirror of guest physical memory for vertex fetch (raw SRV and UAV), with pages copied on demand |
| Vertex upload, 16 MB blocks | Owned by the game thread when the render thread is on, and recycled from completed frames |
| Constant upload, 4 MB blocks | CPU-cached (custom heap, write-back), written with streaming stores and never read back |
| Texture upload, 32 MB blocks | Owned by the untile workers |
| Texture heaps, 128 MB each | Placed textures with a best-fit free list. Past the soft limit, textures are evicted by age, and past the hard limit by recency, once no frame in flight uses them |
| Descriptor heaps | 65,536 views and 2,048 samplers, bindless, with freed views on a free list |

Upload memory is write-combined unless noted, and must never be read on the CPU. Conversions
work in a cached scratch row and copy whole rows out.

## Known gaps

- With the Xenos plugin (F9, or beside the native renderer) occlusion queries still report a fake
  sample count: the SDK's D3D12 backend waits for no result, and the flares show through walls.
- A draw inside an occlusion query that writes nothing and tests neither depth nor stencil is
  skipped, so the query does not count its samples.
- The game renders at 1280x800 natively (a game patch, `pgr4_render_height` in
  [development.md](development.md#native-1610)), filling the 1280x800 panel, never stretched.
  The Xenos plugin, kept as the original renderer, draws that frame with the scene in its top 300
  rows only, so a run that starts on the plugin renders the game's own 1280x720 frame instead
  (`frame_height.h`), and in a 16:10 run the renderer switch stays with the native renderer.
