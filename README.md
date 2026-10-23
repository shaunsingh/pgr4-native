# pgr4-native

A native Direct3D 12 renderer for the Project Gotham Racing 4 static recompilation
([MissingThePixel/MTR-PGR4](https://github.com/MissingThePixel/MTR-PGR4), built on the
[ReXGlue](https://github.com/rexglue/rexglue-sdk) SDK).

The recompiled game normally draws through ReXGlue's Xenos plugin, which emulates the Xbox 360
GPU by replaying its command stream. This renderer hooks the game's own Xbox 360 D3D calls in
the recompiled code instead (197 functions: draws, clears, resolves, Swap and the state calls
around them) and turns each one into D3D12 work.

The goal is 1280x800 at 60 fps under Proton on a Steam Deck.

![The Xenos plugin (left) and the native renderer (right) at the same moment of the same race](docs/race-xenos-vs-native.jpg)

Two runs of the same scripted race, 18 seconds in. The cars take slightly different lines
because the physics is not identical between runs. The native side is missing the car's rear
spoiler, a known bug.

## Summary (2026-10-22)

Measured on an i9-13900K with an RTX 3090, St Petersburg street race, uncapped. Within each row
both columns come from the same build, except the first row's Xenos number, which comes from the
previous build of the same night (the change between them only affects the native path). The
other rows were measured on 2026-10-21 before the recompiler changes below.

| | Native renderer | Xenos plugin |
|---|---|---|
| Windows, release flags | **186 fps** with the render thread | 125 fps |
| Windows, release flags, 2026-10-21 build | 143 fps with the render thread | 126 fps |
| Windows, debug decoder on | 129 fps with the render thread, 96 fps without | 126 fps |
| Linux, Proton, debug decoder on | 112-116 fps with the render thread, 91-92 fps without | 113-114 fps |
| Proton, 8 efficiency cores, debug decoder on | 80 fps with the render thread, 57 fps without | 72 fps |
| CPU cores kept busy (Windows) | about 1.4 with the render thread | about 2 |
| Race image against the Xenos plugin | 31-33 dB PSNR | reference |

The debug decoder is `--pgr4_d3d_tap`, which decodes the game's command buffer at every D3D call
for traces and checks. It costs the native path about 0.8 ms per frame. With it off, native with
the render thread takes 33 % less time per frame than the Xenos plugin (5.36 against 8.02 ms, see
[Windows](#windows)).

- Title screen, menus and the St Petersburg race render. That route is the only one tested.
- The car's rear spoiler is missing.
- The render thread (`--pgr4_native_render_thread`) is on by default. Its wrong-geometry bug is
  fixed: the game thread and the render thread shared one list of pending vertex copies, so
  copies could be lost or reordered. Nine race captures since the fix are all 30.8 to 32.7 dB.
- Runs on a Steam Deck OLED under Proton at 68-74 fps; see [Steam Deck](#steam-deck-oled).
- The game runs at 30 fps by default; MTR-PGR4's `--pgr4_60fps` makes it 60, which has not been
  tried with this renderer yet. The uncapped numbers above measure how much headroom there is.

## Steam Deck (OLED)

Steam Deck OLED in SteamOS desktop mode, GE-Proton 11-5 (vkd3d-proton on RADV), the same Windows
build cross-compiled on Linux (`native/build.sh`: clang-cl and xwin in a distrobox). The game
runs at 1280x800 in a gamescope window on the Deck's panel at 90 Hz with MTR-PGR4's frame-rate
patch, so it presents at most once per 90 Hz vblank. `proton/deck-run.sh` drives the scripted
race and reports frame times, GPU load and the APU's power split (`proton/deck_sampler.py`).

St Petersburg race, 9-45 s after the start (2026-10-23, td-51):

| | |
|---|---|
| Average | 78.7 fps |
| 1 % low | 49.2 fps |
| GPU | 90 % busy at 705-870 MHz (of 1600) |
| APU power | 15 W cap reached in every sample: CPU 8.8 W, GPU 2.7 W, SoC 1.3 W |

The APU's 15 W limit is shared, and the CPU takes most of it, so the GPU runs at about half its
top clock. CPU time saved on any thread becomes GPU clock; GPU work saved shortens the frame
directly. The target is a steady 90 fps with no loss of image quality (MSAA, motion blur and
every other effect stay on, and the frame rate is not capped).

Changes made on the Deck, in order. Each was checked in a race run on the Deck.

- Depth around the car: shaders that write depth no longer scale it into the Xenos plugin's
  float24 range (0...0.5), which halved the depth that depth-restoring passes wrote.
- EDRAM clears: resolve clears cover the stencil plane and land at the right EDRAM offset.
- Per-pass and per-shader GPU timing (`--pgr4_native_gpu_timing`,
  `--pgr4_native_gpu_timing_shaders`, `gpu_timer`).
- The Xenos command processor's thread blocks when its ring is empty instead of yielding in a
  loop, and the timer queue sleeps until its next timer (`native/rexglue-runtime-perf.patch`).
- Guest memory: physical memory is committed with its section, so a page protection change no
  longer costs a commit through wineserver.
- State tracking copies only the register blocks the guest marked dirty.
- Copy segments: vertex copies are recorded ahead of the segment of draws that reads them, one
  pair of barriers per segment instead of one per uploading draw.
- Hot vertex pages: pages the game rewrites every frame are hashed and copied when changed
  instead of being watched and refaulted.
- The frame clock reads the TSC instead of calling QueryPerformanceCounter.
- Resolves copy through compute shaders specialized for each kind of resolve (formats, layout,
  flags, exponent bias compiled in, built on a worker while the generic shader stands in), and
  each resolve phase is timed.
- The display controller's gamma ramp (PWL or table) is applied at present; the image was too
  light without it.
- Pixel shaders are specialized for the textures they sample: when a texture's fetch constant
  is unsigned, or signed in a format whose host view is the same for signed and unsigned, the
  translated shader drops the per-sample sign handling. The motion blur pass went from 1.6 to
  1.0 ms and GPU time per race frame from 13.7 to 10.8 ms.
- `sample_b` (sampling with a LOD bias) translates to a biased sample with implicit gradients
  instead of explicit gradient math.
- `--pgr4_native_shader_dump_dir` writes every translated shader (DXBC) for inspection.
- Shaders are specialized for the guest's bool constants: branches on a bool constant are
  resolved when the shader is translated, so the GPU no longer evaluates them per pixel (about
  2 % of GPU time per race frame; `--pgr4_native_bool_specialize`).
- Pipelines compile on up to four worker threads, and pipelines from the on-disk store are
  built ahead of use and moved to the front of the queue when a draw needs one. When a frame
  brings a burst of new pipelines (the race load brings 150-250), the renderer waits for them
  (at most 500 ms per frame) and does not present a frame with draws still missing, so cars and
  scenery no longer appear late or flicker at the start of a race. One loading pause replaces
  a run of hitches: from a cold store the race's 1 % low went from 26 to 42 fps.
- The pipeline store is safe to use from several threads.
- `--pgr4_native_luma_probe` reads three rows of each presented frame back and logs the
  brightness of each quarter of them in the frame CSV, which finds black or flickering frames
  (or one flashing object) in a run without a capture.
- Textures keep the data their draw was queued with. The render thread reads a texture up to a
  frame after the game queued the draw, and in primary mode the game's fences signal at once,
  so the game may already have streamed new data into that memory. Each draw is stamped when it
  is queued; the first write to an uploaded texture records the latest stamp, and the draws
  queued before it bind the texture as it was. In a race, 100-280 texture bindings per frame
  keep their older data during stretches of the lap (`tex_stale` in the frame CSV,
  `--pgr4_native_texture_capture_order`). Nearly all of them read two textures the game
  rewrites hundreds of times a race (256x512 RGB565 and 512x512 8-bit, read by about 100
  scenery draws a frame); without this, those draws could pick up the next update early, a
  likely cause of buildings flashing dark or bright for a frame.
- Floating point: the Linux build compiles with `-ffp-model=precise -fno-math-errno` instead of
  `strict`. clang-cl ignored the `strict` flag in the Windows builds, so all builds now use the
  same model. Generated code spells out fused multiply-adds with `std::fma`, so contraction does
  not change guest results. No change in frame rate was measured.
- `frame_times.py` also reports a fixed race window (9-45 s after the pre-race grid shot, the
  same span of game frames in every run) and checks the route against a reference
  (`run/route-stpetersburg.draws`, draws per frame each second): a run whose car takes another
  line, or stops against a barrier, reports where it diverged. Runs part ways only from about
  46 s after the grid shot.
- `perf_symbols.py` names samples in Wine DLLs by their nearest export.
- Frame recorder: `--pgr4_native_record` writes the frames shown, at half size and after the
  gamma ramp, from a given frame and optionally in files of N frames (`deck-run.sh --record`).
  `flicker.py` finds what flashed in a recording: blocks that depart from the frames on either
  side for 1-4 frames, or whole-image jumps that come back (`--jumps`), and writes PNG sheets
  of them with the frame CSV's counters for those frames.
- Glitch catcher: `--pgr4_native_glitch_catch` keeps the last 2 s of frames in memory, finds
  the whole-image jumps as they happen and writes the frames around each one; a capture
  request (F12) writes all 2 s. About 5 % of one core. `play.sh` sessions run it and record
  every frame, in files of 5 minutes, so a glitch seen in play can be found afterwards.
- `play.sh` runs the guest's vblank at the OLED panel's 90 Hz (it was 60) and starts Proton
  without xalia, as `deck-run.sh` does.
- `--pgr4_native_coherency_check` (debug, the race runs at about 44 fps with it) keeps a CPU
  copy of what shared memory should hold and checks, per draw, that the vertex and index
  ranges it reads match guest memory, that no copy overwrites a range read earlier in its
  copy segment, which ranges the game rewrites after a draw of the frame read them, that
  shared memory read back after the frame holds what the frame's copies carried, and that the
  render thread's register file matches the one the game thread prepared the draw with. Two
  race runs with it found no stale page, no segment overwrite and no read-back mismatch; the
  only late rewrite is in the front end (6 pages of one vertex fetch range, once).
- Logs: each new pipeline with its compile time, and each 4 MB chunk of shared memory with the
  frame it was mapped at.
- Texture memory is bounded. The texture cache never destroyed a texture: in a 15-minute play
  session it grew to 3.4 GB in 24 heaps of 128 MB (a race uses about 85 MB in 1,000 textures),
  the kernel moved about 3 GB of it from VRAM to GTT, and after 65,000 frames the shader view
  heap (65,536 descriptors) was full and the track stopped drawing. Textures are now held by
  slot and generation handles on a least-recently-used list (touched once per frame) and
  destroyed once no frame in flight uses them: past 512 MB those unused for 30 s, past 1 GB the
  least recently used (`--pgr4_native_texture_memory_soft_mb`, `_soft_lifetime_s`,
  `_hard_mb`). Their ranges in the placed heaps go back to a best-fit free list that merges
  neighbours, and their shader views to a free list. With a 64/128 MB budget a race run
  evicted 5,346 textures with a correct image, at most 1.2 ms per frame. The frame CSV has
  `tex_mb`, `tex_count`, `tex_evicted`, `tex_evict_us` and `views`.
- No frame is shown with draws missing. A draw whose pipeline is still compiling is skipped;
  the frame is then held (the previous one stays on screen) and waits after its last draw for
  all of its pipelines at once, at most 0.5 s, compiled on all but two hardware threads
  (`--pgr4_native_pipeline_wait`, which replaces `--pgr4_native_pipeline_burst`). Before,
  frames with fewer than 8 new pipelines were shown without their draws for 2-5 frames (207
  race frames in a cold-cache run: objects flickered and buildings turned black or white), and
  in a burst each draw waited for its own pipeline, so the pipelines compiled one after
  another (30-40 ms each in the play session, several frames at the 500 ms limit). In a
  cold-cache run (no vkd3d, Mesa or native pipeline cache, frames recorded) no frame was shown
  with a draw missing; the race's 79 new pipelines held 48 frames for 2.4 s in total, 70-80 ms
  each (1 % low 13.5 fps, against 26.9 when the frames were shown incomplete). With warm caches
  nothing changes.
- Marks: clicking both sticks (L3+R3) on any pad marks the moment as F12 does (the glitch
  catcher's last 2 s, a PNG of the frame and a log line with the frame), so a back grip can mark
  through a Steam Input binding that sends both. F12 is now read as the key's state: R4 bound
  to F12 had marked once in a whole play session, as GetAsyncKeyState's "pressed since the last
  call" bit was missing for most presses under Proton.
- `deck-run.sh` keeps a `VKD3D_SHADER_CACHE_PATH` set by the caller (0: none), for
  cold-cache runs with `MESA_SHADER_CACHE_DISABLE=true` and
  `--pgr4_native_pipeline_cache=false`.
- A held frame is not drawn to the back buffer and not recorded, so recordings, the glitch
  catcher and the luma probe see only what the screen showed. 58 of the play session's 86
  glitch dumps were held frames nobody saw (frame 23024, where the Shanghai buildings were
  missing, was one); in a cold run's recording exactly the 39 held frames are missing.
- New bool and texture-sign specializations of known shaders no longer hold frames. A draw
  whose specialization is new uses the pipeline of its unspecialized shaders when that one is
  ready, and the specialization compiles behind the pipelines frames wait for
  (`--pgr4_native_generic_pipelines`, CSV `pso_generic`). The first draw of a shader pair and
  state still waits, for its specialization (it has less code and compiles faster), and queues
  the unspecialized pipeline. The play session's overtake race brought about 3,500 new
  pipelines, most of them new bool/sign combinations of known shaders, and held frames for
  5-8.5 s per 30 s. Waiting for the unspecialized pipeline instead, as a first version did,
  held fewer frames in a cold race (15 against 40) but each for twice as long (100-190 ms).
  Now, cold (td-18 against td-15): the race window held 16 frames for 0.85 s in total (40 for
  2.0 s), the whole run waited 6.0 s (8.1), and the race's 1 % low was 15.3 fps (13.5). The
  intro frames drawn partly on unspecialized pipelines look like those of the run without.
  Cold runs leave the route after 15 s, so their averages are not comparable.
- Marks in play sessions write the native image alone, from a thread, once the frame's fence
  has passed (`--pgr4_native_capture_xenos=false` in `play.sh`). A mark had held the game
  thread for two Swaps, switched the Xenos plugin to full readback and written two PNGs on the
  game thread, for a Xenos copy that is meaningless in primary mode: a frame of 104-749 ms per
  mark in the play session. A capture mid-race now made the next four frames 18-26 ms.
- `marks.py` shows what the screen showed in the 5 s before each mark of a play session (a
  mark comes a few seconds after what it marks): from the session recording, the slow frames
  with their causes, the blocks that flashed and the whole-image jumps, each with its time
  before the mark, as PNG sheets. With a recording running, a mark no longer writes the glitch
  catcher's 2 s as well. In the play session's 19 marks it found geometry exploding into
  screen-wide triangles and shards for single frames (a cockpit replay, AI cars, the garage),
  with clean frame counters: the next bug to fix.
- Most of those frames are PGR4's look-back camera, which R3 (the right stick clicked) shows
  while it is held: in the chase view the camera turns round to face the car's headlights, in
  the cockpit it looks out of the rear window past the driver's arm. The mark chord L3+R3
  reached the game too, so each mark, and each press too short for both buttons to show in one
  frame, looked back for 1-6 frames; marks 25767, 41499, 91103 and 92537 each have one at the
  mark itself. In a scripted race, R3 held for 300 ms and for 1.5 s looks back as long (td-19).
  Marks are F12 alone now, a key the game has no binding for. The frame CSV has the pad as the
  game read it (`pad_buttons` in hex, `pad_lt`/`pad_rt`, `pad_lx`..`pad_ry` as signed high
  bytes; from XamInputGetState, buttons OR'd over a frame's reads and each axis at its largest
  deflection), so a flash can be checked against the input.
- The snow race's flashing frames (Quebec, Subaru 22B): for one or two frames the scenery
  broke into grey slabs, stripes and smears, or turned black, while the car and HUD stayed
  right; Xenos drew the same frames correctly (td-34). Neither serializing the GPU, recording
  on the game thread, unlimited texture memory nor turning off stand-in pipelines stopped it
  (td-35). A draw log of 3,265 race frames caught one (td-38, frame 23252): against its
  neighbours, the only change was the vertex shader of 240 draws, with the same address, size,
  buffers, textures and state. When the D3D binds a vertex shader variant with another vertex
  declaration and the GPU has finished the frames that loaded it, it patches the variant's
  vertex fetches in place (sub_826A9718 -> sub_826A8C20) instead of loading a patched copy
  through the push buffer. The renderer's shader memo, keyed by address and size and checked
  against the first and last eight dwords, kept the shader translated before the patch, and
  the draws read their vertices in the wrong layout. Whether the D3D patches in place depends
  on how far the GPU is behind, so the frames came and went with timing. The state tracker now
  reads the variant's binding record (declaration id and stream layout) into the shader
  reference, and the memo misses when it changes. Each run logs its first eight in-place
  rebinds (`rebound in place at frame N`); td-40 had its eight before the race started. Its
  130 s of the snow race (driven by hand once the scripted route had left the road) caught no
  flashing frame, at 85 fps average and a 1 % low of 48 fps.
- Debug switches used to find it: `--pgr4_native_gpu_serialize`, `--pgr4_native_draw_log_from`
  and `_draw_log_frames` (draw logs of N frames, with each draw's index and vertex buffers and
  a hash of the float constants each shader reads; resolves, EDRAM fills and render target
  creation are logged between the draws), `--pgr4_d3d_tap_check`. `deck-run.sh --draw-log N`
  logs N frames from the race start; `--race-draws` sets the median draws per frame that
  shows the race is on (3000, St Petersburg's; the snow route runs with 1000).
- Title Update #2 (0.0.2.1, the game's last version): the build recompiles the disc's
  executable with the update applied, so World Challenge mode is in the Single Player menu and
  the game reads downloadable content (`title-update/README.md`). The update, free on Xbox Live,
  ships in `title-update/`; codegen applies its `default.xexp` to a copy of the disc's
  `default.xex`, and the runtime mounts the folder as `update:\` and refuses an image other than
  the one the code was generated from. MTR-PGR4's patches to the generated code name base
  addresses: `native/tools/tu_address_map.py` matched the two executables' functions
  instruction by instruction (`title-update/addresses.txt`), the build rewrites MTR's patches
  and manifest entries with that table, and the garage step-count patch is rewritten for the
  register the update keeps the count in. The renderer's own hooks into the D3D library moved
  by the same map. DLC copied off a console as STFS packages goes in the game folder's
  `Content\` (or `--content_root`); the runtime installs each new package into the user data's
  content store at startup (td-41: Vehicle Pack 1 installed, World Challenge in the menu).
- Play sessions are lean by default. The 15-minute play session (195,000 frames) had 70 stalls
  over 100 ms, up to 2.3 s, spaced at multiples of 5 s: `play.sh` recorded every frame shown
  (about 50 MB/s, 112 GB in the session), and the kernel's writeback paused the game's writing
  threads (the frame CSV and the log among them). The scripted snow race without a recording
  (td-39) had none. `play.sh` now starts the game alone, as a console would: no frame CSV, GPU
  pass timing, sampler, glitch catcher or marks, and the game log, Wine and vkd3d-proton keep
  warnings and errors only. `play.sh --debug` (or `PGR4_DEBUG=1`, and a second desktop entry) is
  a debugging session with all of them; `PGR4_RECORD=1` adds the recording. The glitch catcher
  keeps 5 s (it was 2 s), since a mark comes a few seconds after what it marks.
- Capture requests are polled only when `--pgr4_native_capture_dir` is set; it has no default
  folder now. Every frame had queried `capture.request` through Wine's path translation and
  read the F12 key, and every 15 frames opened `draw-debug.txt`. The request file is now read
  every 16th frame (td-44 took its 13 captures). `run.ps1 -Native` passes the Windows lab's
  folder.
- The title screen's Ferrari (its diffuser on the white floor, where play showed white
  shimmering): 13 oracle captures of the slide-in (td-44) differ from Xenos by more than 48
  levels in 2-19 pixels each (PSNR 45-49 dB). In a recording of 1,800 frames (td-45) the
  diffuser's edge holds still from frame to frame (mean luma change 0.14, p99 2); the white that
  comes and goes there is the title screen's rain, streaks of one or two frames that Xenos draws
  too.
- Pass-through D3D hooks jump straight to the game's function. Of the D3D library functions the
  renderer hooks, the ones neither it nor the state tracker needs (`IsPassThrough`) still went
  through `OnHook` on every call to be counted. They now return through the original unless the
  tap or the profiler is on; `RestorePush` keeps the full path, since a held `BeginVertices`
  draw is queued at its entry. `OnHook` was 3.4 % of the game thread (td-46) and is gone from
  td-47's profile. The frame rate did not move (race 69.1 fps against 68.8): the APU is at its
  15 W limit in every sample, the CPU takes 9.1 W of it and the GPU runs at 600-685 MHz, 86 %
  busy. The frame waits on the GPU, and the CPU's savings are power the GPU can clock up with.
- The XMA decoder's worker no longer wakes after every kick. A kick (the game setting a
  context's kick bit) decodes the kicked contexts at once on the game's audio thread (MTR's
  change), and `Work` takes the context's enable, so the worker woken behind it walked all 320
  contexts, about 18 KB apart and a cache miss each, and found nothing: 71 % of its 8.6 % of a
  core sat on the load of the enable flag (td-47). The kick no longer sets the worker's event
  (`rexglue-runtime-perf.patch`); pause and shutdown still do. td-48: the thread is idle, the
  game's threads take 260 % of a core (277 %), the audio thread 47 % (52 %); race 70.6 fps
  (69.1), 1 % low 41.9 (36.3), p99 20.8 ms (23.6), GFX clock 716 MHz (685).
- `deck-run.sh --pmu` (with `--perf`) counts the front end per thread over the perf window
  (`proton/pmu_summary.py`). td-48: the game thread retires 1.16 instructions per cycle and
  refills its instruction cache 26 times per 1000 instructions (22 from L2, 3.8 from memory);
  the render thread 63 times (58 from L2). The recompiled code does not fit the caches, so its
  layout is worth more than any one function.
- The game is built with profile-guided optimization. 90 % of the executable's samples fall in
  677 functions spread over 35 MB of its 38 MB, and of their 2.9 MB only 480 KB of cache lines
  run. With a profile, clang puts each function's hot blocks together, its cold ones after
  them, and inlines by call counts. `native/build.sh --pgo gen` builds the game instrumented and
  `--pgo FILE` with a profile; a build without `--pgo` uses `<dest>/pgo/default.profdata` when
  it exists. The Linux LLVM packages have no Windows profile runtime, so `build.sh` compiles
  compiler-rt's for the clang-cl in use, and as the run scripts kill the game, the instrumented
  build writes its counters every 5 s (`src/native/pgo_counters.cpp`). The profile is one
  scripted race run (td-49, 63 fps while counting). td-50 against td-48: race 75.7 fps (70.6),
  1 % low 44.7 (41.9), p99 19.7 ms (20.8), GFX clock 772 MHz (716); the game thread refills its
  instruction cache from L2 16.5 times per 1000 instructions (22.1) at 1.21 instructions per
  cycle (1.16), the render thread 50 times (58).
- A new XMA stream no longer reopens its decoder. When the game frees or clears an XMA context,
  MTR's fix for a sound's tail bleeding into the next one cleared the decoder's cached format,
  so the stream's first frame freed the FFmpeg context and opened another: about 450 KB
  zeroed and MDCT tables for seven block sizes (up to 8192 points) rebuilt, about 55 times a
  second in a race. FFmpeg's xmaframes decoder now has a flush that puts each channel back as
  `decode_init` leaves it (overlap tail, previous block length, scale factor history), and a
  reset calls it (`rexglue-runtime-perf.patch`). td-52 decoded each new stream's first 64
  frames through both the flushed and a reopened decoder: 105,000 frames from 3,128 streams,
  menus and race, and not one sample differed. `ff_mdct_init` and `split_radix_permutation`,
  2.5 % of the audio thread in td-50, are gone from td-53's profile. The frame rate cannot
  show a change this small: td-54 ran the race at 74.9 fps (td-51: 78.7) at a lower GPU clock
  (774 MHz against 870, 2.5 W against 2.7), with one burst of 11 slow frames after the car left
  the scripted line, and its audio thread took 44.5 % of a core (44.2).
- Wineserver requests per thread. Under Proton, many calls that Windows handles in its kernel
  are requests to wineserver, a separate process: two context switches and a CPU wakeup each,
  paid for out of the APU's power budget. `deck-run.sh --server-trace` runs wineserver with its
  request trace and `proton/server_requests.py` counts each thread's requests per second over
  the race window. td-60: 17,700 requests a second. The game thread made 1,100 of them: 380
  `ResumeThread` calls on worker threads that were not suspended, 77 `GetThreadPriority`
  queries, and a rumble report (an event, a HID ioctl and a close) every frame. Without the
  frame-rate cap (`--uncapped`, td-55/56) the race ran no faster, with the GPU 93 % busy: the
  frame waits on the GPU, and the GPU's clock waits on the power the CPU leaves it.
- The game thread no longer makes those calls (`rexglue-runtime-perf.patch`). Resuming a guest
  thread that is not suspended does nothing, as on the console; the count had also wrapped
  below zero, which lost the thread's next suspend. A thread's priority is read from the value
  last set. The guest's motor speeds are stored and sent by the UI thread with its next pump,
  and SDL sends nothing when they did not change. td-61: the game thread made 167 requests a
  second (1,103), and the race ran at 78.0 fps (77.1). `--kernel_wait_stats` logs, per guest
  thread every 5 s, how long its kernel waits blocked and which guest code called them.
- Mid-frame submission. The render thread submitted each frame as one command list at the
  Swap, so the GPU sat idle from the end of one frame until all of the next was recorded
  (2.2-2.9 ms per frame in td-62), and the render thread then waited in Present. Now every 256
  draws (`--pgr4_native_submit_draws`, 0: once per frame) the frame so far is submitted, once
  the untile workers have filled the upload memory its texture copies read, and recording goes
  on in a fresh list. td-65 against the same build with it off (td-66), race: 80.0 fps against
  76.1, 1 % low 51.2 against 39.5, no hitches against 6, Present 0.9 ms per frame against 3.2,
  the game thread waiting for the render thread 0.06 ms against 1.07, 12 submissions per frame.
  The game thread is now the limit (92 % of a core), and the GPU still waits 1.0-1.6 ms per
  frame for the next list.
- The pad is read on a thread of its own. ReXGlue's SDL driver had every XInput call of the game
  ask the UI thread to pump SDL's events, and an event watch copied every joystick and gamepad
  event (each axis of each HID report) into a queue that the game's next call replayed under the
  driver's lock. Now an SDL timer reads each pad's buttons and axes every 4 ms and keeps the
  result, the game's calls copy it under the lock, SDL no longer queues the per-report events,
  and the event filter keeps only connects and disconnects; rumble goes out from the timer
  thread, outside the lock (`rexglue-runtime-perf.patch`). A first version filtered out the
  joystick events from which SDL's gamepad layer builds its own state, so a connected pad read
  as idle; the timer now asks the pad for its state instead of relying on events. td-70 against
  td-65: the UI thread took 1.4 % of a core (9.0), the game thread that polls the pad 14.4 %
  (19.9), the timer 5.3 %. td-73 (wineserver trace): race 80.8 fps, 1 % low 49.7 (td-65: 80.0,
  51.2). Of its 16,400 wineserver requests a second, 13,000 are the pad's: winedevice's HID
  threads (9,200), SDL's reads (2,300) and Wine's own XInput reader (1,700), which reads the
  same pad as SDL; winedevice takes 21 % of a core.
- The state tracker hands the render thread its register changes as a list. Each write that
  changes a register's value is appended as it happens, and `TakeChanges` copies the list in
  order; before, each write set a bit in a two-level bitmap, and `TakeChanges` walked the bitmap
  and read every marked register back. The register blocks keep their group count instead of
  dividing for it at every draw, and the texture cache's capture stamp, which only the game
  thread writes (once per draw), is a plain store on a cache line of its own instead of a
  locked add beside members the render thread reads for every texture it binds. td-73 runs
  both; no run measures them on their own.
- The continuous recorder is gone (`--pgr4_native_record`, `play.sh`'s `PGR4_RECORD`,
  `deck-run.sh --record`). It wrote every frame shown to disk at half size, about 50 MB/s,
  which stalled the game, while the glitch catcher already keeps the last 5 s in memory and
  writes them out at each whole-image jump and each F12 mark; `marks.py` and `flicker.py` read
  those dumps.
- `deck_sampler.py` snapshots every process's CPU time every 5 s, and its summary lists the
  busiest processes besides the game: what else draws on the APU's 15 W. td-72 ran the same
  build as td-73 while this session's agents worked beside it: race 78.2 fps, 1 % low 40.6, 6
  hitches, GPU clock 737 MHz, against 80.8, 49.7, 4 and 771 MHz; the session took about a
  quarter of a core. `perf_symbols.py --annotate TEXT` lists the busiest instructions of the
  functions whose name contains TEXT, by offset, for `llvm-objdump`.
- A cleanup pass over the renderer's C++, with no change in what it draws. Dead code went: draw
  state fields and device offsets nothing read, the render targets' and resolver's own
  `OnClear`/`OnResolve` paths and counters, coherency counters nothing reported, unused `base`
  parameters (and the renderer's copy of the guest memory base, stored at every hook call for
  one debug dump), and the viewport math of draws dropped before they use it. Guest memory
  reads go through one header (`guest_memory.h`: the SDK's host address translation and
  byte-swapped loads) instead of four copies, and barriers, buffer descriptions, committed
  buffers and the D3DCompile loader through helpers in `d3d12_device.h` instead of a dozen
  hand-written copies. The glitch catcher's parts lost the removed recorder's names, the PNG
  CRC table is built at compile time, arrays come from `make_unique` (the 512 MB coherency
  shadow from `make_unique_for_overwrite`), and the debug toggle the game thread reads is an
  atomic. td-74: race 82.4 fps, 1 % low 56.6, 0 hitches, GPU clock 816 MHz (td-65, the last
  run without a server trace: 80.0, 51.2, 0). It is also the first run of the input and
  tracker changes without the trace, so no run measures the cleanup on its own. The PGO
  profile is stale in the changed functions, which clang compiles without it, until it is
  taken again.
- `native/src` is formatted by `native/.clang-format`: ReXGlue SDK's (Google style, 100
  columns) with Google's own short functions, if statements and loops on one line. Run
  `clang-format -i` on the files a change touches (the distrobox has it). The formatting
  commit is in `.git-blame-ignore-revs`.

## Why a native renderer can beat emulation
- PGR4 renders with predicated tiling. The Xbox 360 GPU draws into 10 MB of EDRAM, and the game
  splits each frame into screen tiles that the GPU replays the draws for. In the race the game
  makes about 3,300 draw calls per frame, and the Xenos plugin executes about 5,500 GPU draws
  because it runs them for two tiles plus an untiled pass. The native renderer sees each D3D
  call once and draws it once at full size.
- With the Xenos plugin, the game thread and the plugin's command-processing thread keep about
  two cores busy. The native renderer starts from the D3D calls and costs about 0.9 µs of CPU
  per draw. With the render thread, that work moves off the game thread onto a second thread
  that is about 43 % busy.
- The game's own code sets the floor for both renderers. Turning on ReXGlue's register-as-local
  codegen options (`cr`, `ctr`, `xer`, `non_volatile` and `reserved_as_local`) keeps guest
  registers in C++ locals instead of the context struct in memory. That took the native race
  from 7.02 to 5.74 ms per frame, 17.9 % less (paired A/B, 95 % interval 14.2 to 21.6 %). The
  Xenos plugin did not get faster, because its command-processing thread is the limit there
  (98 % of a core). `native/build.ps1` explains why `skip_lr`, `skip_msr` and
  `non_argument_as_local` stay off for this game.
- Part of that floor is the game's D3D library. Each hook calls the original D3D function
  first, and that function still builds the command buffer the Xenos plugin would read.
  UnleashedRecomp and MarathonRecomp replace those functions instead of calling them. The first
  piece of that is here, `--pgr4_native_replace_draws` (on by default since 2026-10-22): in
  primary mode DrawIndexedVertices, 91-98 % of the game's draws, no longer builds command packets.
  On the same exe it took frame time from 6.08 to 5.55 ms, 8.5 % less (95 % interval 2.5 to
  14.4 %), and a recorded race next to the Xenos plugin showed no image difference beyond the
  known spoiler bug.

## Performance

### How it is measured

- A key script drives the game from boot through a new profile to the St Petersburg street race
  (Arcade, chapter 1, event 1) and holds the throttle.
- Frame time is the average over the 40 seconds after the race starts, taken from the game's own
  log (`native/tools/perf_fps.py`). Runs are uncapped (`--vsync=false
  --d3d12_present_vsync=false`), which does not change PGR4's game speed.
- Unless a table says otherwise, runs have the debug decoder (`--pgr4_d3d_tap`) on.
- Two runs of the same build differed by 8 % in one test, because the scene at a given second
  changes a little from run to run. To compare two builds or two flag sets, runs alternate
  (A, B, B, A, ...) and `native/tools/ab_compare.py` compares them in 4-second bins at the same
  point of the race. One pair of runs gave a 95 % interval of about ±3 %.

### Windows

| Configuration | ms per frame | fps |
|---|---|---|
| Native, render thread on, debug decoder off | 6.99 | 143 |
| Xenos plugin, debug decoder off | 7.94 | 126 |
| Native, render thread on | 7.76 | 129 |
| Xenos plugin | 7.95 | 126 |
| Native, render thread off | 10.46 | 96 |

Paired comparisons on the same build:

- Release flags (decoder off), Xenos against native with the render thread: native takes
  12.6 % less time per frame (95 % interval 3.5 to 21.8 %, one pair of runs). The gap is 2 to
  2.5 ms in the busy stretches of the race and close to zero in the light ones, where the game's
  own code sets the frame time for both.
- Turning the render thread on (decoder on) takes frame time from 11.09 to 8.16 ms, -26.3 %
  (95 % interval -29.3 to -23.2 %).

How the native frame rate got there, with the Xenos plugin on the same build for reference:

| Change | Native fps | Xenos fps |
|---|---|---|
| First build where the native renderer is the game's output | 46 | 130 |
| Texture conversions stop reading write-combined upload memory (14 ms per frame); textures placed in heaps; untiling and pipeline compiles on worker threads | 92 | 123 |
| Texture binding memo, draw-path memos, cached constant buffer memory, persistent pipelines | 100 | 130 |
| Render thread | 129 | 126 |

Per-draw CPU cost in steady race frames, before the render thread: textures 218 ns, constant
buffers 175 ns, vertices 120 ns, state tracking 115 ns, command recording 113 ns.

### Linux and Proton

GE-Proton 11-11 (vkd3d-proton) in a Hyprland session on the same PC, running the same Windows
build.

| Configuration | ms per frame | fps |
|---|---|---|
| Native, render thread on | 8.65-8.94 | 112-116 |
| Xenos plugin | 8.77-8.86 | 113-114 |
| Native, render thread off | 10.88-10.99 | 91-92 |
| Native, render thread on, 1280x800 window | 8.72 | 115 |

- With the render thread on, native and Xenos are level under Proton: -3.3 % (95 % interval
  -10.1 to +3.5 %). With it off, Xenos takes 17.5 % less time per frame than native.
- Against Windows, Proton makes the Xenos plugin about 11 % slower and native with the render
  thread 8-13 % slower. Native without the render thread is within 5 %.
- The race image check gives 31.5-31.7 dB, the same as on Windows, and vkd3d-proton logged no
  errors.
- The 1280x800 window runs at the same speed as 1280x720.

### A slower CPU

There is no Steam Deck to test on yet. As a rough stand-in, the game was pinned to eight of the
13900K's efficiency cores (eight threads, no SMT) under Proton. The GPU is still an RTX 3090, so
this only says something about the CPU side. A Deck's Zen 2 cores are slower again.

| Configuration | ms per frame | fps |
|---|---|---|
| Native, render thread on | 12.50 | 80 |
| Xenos plugin | 13.96 | 72 |
| Native, render thread off | 17.69 | 57 |

On these cores native with the render thread takes 14.7 % less time per frame than Xenos (95 %
interval 5.9 to 23.6 %), and the render thread cuts native frame time by 30.8 %.

### Hitches

- The frame where the race starts went from 357 ms to 76 ms with cold caches and 84 ms with warm
  ones.
- A run from boot to the race has 3 frames of 50 ms or more, down from 9. Two of the three are
  start-up and a loading screen.
- What fixed it: texture conversions no longer read back write-combined memory (98 to 2.8 ms for
  a 1280x720 depth upload); textures are placed in heaps instead of created one by one (138 to
  9.5 ms for 627 textures); untiling and pipeline compiles run on worker threads; upload blocks
  are reserved at start-up; polygon offset is dynamic state instead of part of each pipeline
  (957 to 316 pipelines in a cold run); pipelines persist across runs.
- Still left at race start: first vertex copies (14-26 ms), untiling work the first submit waits
  for (about 48 ms), and shader translation in a cold run.

## Image accuracy

The image check runs both renderers side by side (`--pgr4_native`, `--readback_resolve=fast`),
captures both front buffers at three race frames and compares them. A capture passes at 30 dB
PSNR or more. Race frames score 31-33 dB and menus 34-40 dB.

![Native (left), Xenos (middle) and their difference x8 (right) for one race frame](docs/race-difference.jpg)

`native/tools/oracle_diff.py` makes images like this one and scores each 80x80 tile and each
40-row band. What it shows:

- The worst tiles are around the player's car (17-19 dB, against a median tile of about 46 dB),
  where the spoiler is missing.
- The difference jumps at row 480. In the six race captures checked (three on Windows, three
  under Proton), the mean difference per pixel is at most 1.1 (out of 255) in each 40-row band
  above row 440, and 3.3 to 8.2 in each band from row 480 down. The game renders two screen
  tiles plus an untiled pass per frame, and a first tile of about 480 rows would fit in the
  EDRAM, so this may be the tile boundary. Not verified yet.
- With the render thread on, one capture in three drops to 29.5 dB. In one it is an extra dark
  polygon in the sky, in another a bright band along the far barriers. Both are wrong geometry,
  so the likely cause is vertex or register data reaching the render thread late or out of
  order.

## Bugs found on the way

| Symptom | Cause | Fix |
|---|---|---|
| Car garbled, crowd missing | The game writes a patched copy of each vertex shader (matched to the vertex declaration) into the command buffer; the renderer used the original microcode | Copy the patched microcode in a hook |
| Frame turns white after tone mapping | ReXGlue's shader translator numbers texture and sampler slots from 1; the binding table was filled from 0 | Size the table to the highest slot plus one |
| Image check stuck near 25 dB | Fast readback returns the previous resolve | Hold a full readback for the compared frame |
| One cube map face at 5 dB | The Xenos plugin packs each 7e3 float sample (flushing values below 2^-9) before averaging MSAA samples; the renderer averaged first | Pack each sample before averaging |
| Lost frames at the title screen | The game creates a second D3D device | Follow the device that Swap and the draws use |
| Clip plane mismatches | The clip-plane register block is 8 registers and the planes flush from a different device field | Map the block correctly |
| 14 ms per frame lost | Texture untiling wrote into write-combined upload memory and the format conversion read it back (about 55 ns per byte) | Convert in a cached scratch row, then copy whole rows out |
| Depth bias missing on some draws (found by reading the code, not seen in the image check) | `SetPipelineState` loads the pipeline's depth bias even when the pipeline uses dynamic depth bias, and the renderer kept its cached value across pipeline changes | Reset the cached bias after each pipeline change |

## How it works

- `native/src/pgr4_d3d_trace.cpp` hooks the guest D3D functions listed in
  `native/src/d3d_surface.inc`. Each hook calls the original guest function first, so the guest
  D3D state and the Xenos plugin stay correct, then passes the call to the renderer.
- `d3d_state` tracks the Xenos register state the guest D3D device writes for each draw.
- Shaders: Xenos microcode goes through ReXGlue's DXBC shader translator
  (`shader_translate`, `shader_cache`).
- Textures (`textures`, `texture_layout`): guest textures are untiled on CPU worker threads into
  upload memory and copied into placed resources. Guest writes are caught with page watches.
- Render targets and resolves (`render_targets`, `resolve`): the 360's EDRAM targets become host
  render targets; resolves copy them into host textures that later draws sample.
- Pipelines (`draw`, `pipeline_store`): pipeline states compile on a worker thread and persist
  across runs in an `ID3D12PipelineLibrary` under the runtime's cache folder.
- With `--pgr4_native_render_thread` (the default), the game thread only captures each call and
  its state; a render thread records and submits the D3D12 work.
- `present` presents the guest's front buffer into the game window or a debug window.

Several files port Xenia/ReXGlue code (BSD-3-Clause), among them `draw`, `render_targets`,
`resolve`, `geometry_shader` and the texture code. Their headers name the ported functions.

## Building

You need Windows 10 or 11, Visual Studio 2022 Build Tools (x64), LLVM (clang-cl), CMake, Ninja,
PowerShell 7 and Git. You also need the files from your own PGR4 disc, extracted to a folder
(`default.xex`, `gw.xex`, `Game\`, `UI\` and so on). This repository contains no game data, and
the recompiled C++ that the build generates from `default.xex` must not be published.

1. Build MTR-PGR4 and its patched ReXGlue SDK:
   `pwsh -File build\build-mtr.ps1 -Root C:\recomp\builds\pgr4 -Game <disc folder> -Stage prep,sdk`
2. Build the renderer into its own tree. This copies the MTR-PGR4 checkout, applies the two SDK
   patches, adds the hooks and `native\src\native\` to the game, then builds:
   `pwsh -File native\build.ps1 -Root C:\recomp\builds\pgr4 -Game <disc folder> -Dest <tree>`
3. Run it: `pwsh -File native\run.ps1 -Action start -Dest <tree> -Game <disc folder> -Native -Extra "--pgr4_native_primary"`.
   Add `--pgr4_native_render_thread=false` to the `-Extra` flags for the single-thread path. `run.ps1`
   starts the game in the interactive desktop session through a scheduled task, so it also
   works over SSH. `-Action stop` closes it. The game's executable is
   `<tree>\run\runtime\pgr4_recompiled.exe`. `-Tap` turns on the debug decoder
   (`--pgr4_d3d_tap`), which costs about 0.8 ms per frame.

`build.ps1` expects `native\` and `instrument\` side by side, as in this repository.

Under Proton the same Windows build runs as it is: start `pgr4_recompiled.exe` with
`proton run` and the game flags that `run.ps1` passes.

## Flags

| Flag | Default | Effect |
|---|---|---|
| `--pgr4_native` | false | Run the native renderer beside the Xenos plugin, in a debug window |
| `--pgr4_native_primary` | false | The native renderer is the game's output; implies `--pgr4_native` |
| `--pgr4_native_render_thread` | true | Record and submit on a render thread; the game thread only captures each call |
| `--pgr4_native_async_pipelines` | true | Compile new pipelines on worker threads; the draws that need them are skipped |
| `--pgr4_native_pipeline_wait` | true | A frame that skipped draws is not shown and waits for their pipelines (at most 0.5 s) |
| `--pgr4_native_generic_pipelines` | true | A draw whose bool/sign specialization is still compiling uses its unspecialized shaders' pipeline when that one is ready |
| `--pgr4_native_pipeline_cache` | true | Keep pipelines across runs |
| `--pgr4_native_dynamic_depth_bias` | true | Set polygon offset per draw instead of per pipeline |
| `--pgr4_native_submit_draws=<n>` | 256 | Submit the frame so far every n draws, so the GPU starts on it while the rest is recorded (0: once per frame) |
| `--pgr4_native_csv=<file>` | off | Per-frame counters and CPU times |
| `--pgr4_native_profile` | false | Per-stage CPU time of the draw path, added to the CSV |
| `--pgr4_native_capture_dir=<dir>` | off | Where image-check captures go (native and Xenos PNGs, `captures.csv` with PSNR); a `capture.request` file there or F12 takes one, and `draw-debug.txt` there is read. Off, none of them is polled (`run.ps1 -Native`: `C:\recomp\logs\pgr4-native-captures`) |
| `--pgr4_native_capture_frame=<n>` | -1 (off) | Capture the front buffers at this native frame |
| `--pgr4_native_capture_xenos` | true | Captures also read the Xenos copy and its PSNR, holding the game thread; false: the native PNG alone, written from a thread |
| `--pgr4_native_glitch_catch=<prefix>` | off | Keep the last 5 s of frames; write whole-image jumps and F12's 5 s (with a capture folder) to `<prefix>glitch-<frame>.rec` / `mark-<frame>.rec` |
| `--pgr4_native_coherency_check` | false | Debug: check vertex/index data and registers against guest memory (slow) |
| `--pgr4_native_gpu_serialize` | false | Debug: wait for the GPU at the end of every frame (slow) |
| `--pgr4_native_draw_log_from=<n>` | -1 (off) | Debug: log every draw of `_draw_log_frames` (1) frames from native frame n to `<capture dir>/draws-f<frame>.txt`; bit 16 of `<capture dir>/draw-debug.txt` does the same from the next frame |
| `--pgr4_d3d_tap_check` | false | Debug, with `--pgr4_d3d_tap`: compare the tracked state with the decoded push buffer at every draw and log the frames that differ (slow) |
| `--pgr4_native_texture_memory_soft_mb` | 512 | Above this, textures unused for `_soft_lifetime_s` (30) are destroyed (0: no limit) |
| `--pgr4_native_texture_memory_hard_mb` | 1024 | Above this, the least recently used textures are destroyed (0: no limit) |
| `--content_root=<dir>` | `<game>\Content` | Content packages (DLC as STFS files off a console) installed at startup |

With `--pgr4_native_primary`, the Xenos plugin only runs fences and interrupts
(`--gpu_skip_draws`, added by `native/rexglue-native-primary.patch`). The debug flags not in
the table (resolve dumps and self-test, the watch, texture and state tracker checks, the tap's
trace, frame CSV and draw skips, the debug window's size) are described where they are defined,
in the `REXCVAR_DEFINE_*` lines of `native/src`.

## Tools

`native/tools/` has the scripts used to measure and check the renderer:

| Script | What it does |
|---|---|
| `perf_fps.py` | Frame time from the game log over a time window |
| `ab_compare.py` | Two builds or flag sets compared in matched 4-second bins of the same route, with a 95 % interval |
| `oracle_diff.py` | Side-by-side, heatmap, per-tile and per-row scores for native and Xenos captures of the same frame |
| `perf_stats.py` | Stage costs per draw from the CSV |
| `perf_hitch.py` | Slow frames and their causes |
| `compare_resolves.py` | Resolve outputs against the Xenos plugin's |
| `flicker.py` | Flashing blocks or whole-image jumps in a glitch catcher dump, as PNG sheets |
| `marks.py` | The seconds before each mark of a play session: slow frames, flashes and jumps, as PNG sheets |

It also has a texture untiling harness and an offline shader translation test. `proton/` has the
Deck's: `deck_sampler.py` (GPU load and clock, the APU's power split, the busiest threads and
other processes), `pmu_summary.py` (`deck-run.sh --pmu`: instructions per cycle, fetch stalls
and instruction cache refills per thread) and `server_requests.py` (`deck-run.sh
--server-trace`: wineserver requests per thread and second, by request).

## What's next

- Check the other single-frame geometry glitches against the in-place rebind fix: about 10 s
  into the St Petersburg race, in about one run in three, the sky turned red and the far
  buildings broke into shards while near geometry stayed right. The single frames of play
  where the cockpit turned white and broke up were the look-back camera; whether its view of
  the rear interior is right is still to be compared with Xenos (the chase view's look-back
  matches it at 45-47 dB, td-21). Shards near AI cars and in the garage need a play session
  with the pad column to tell them from look-backs.
- Find out whether the row-480 jump is the tile boundary, and whether it explains the spoiler.
- Steam Deck: a steady 90 fps. Less CPU time per frame (it takes most of the 15 W) and less GPU
  work per frame.
- Cut the remaining race-start work: first vertex copies, untiling the first submit waits for.

## Tooling and agents

The renderer was built and tuned by coding agents, several at a time, driving one Windows PC.
The tooling for that is in this repository:

- `tools/lab/`: the `lab` CLI (Bun). `lab pgr4 <verb>` syncs this checkout (or a commit) to a
  build tree on the Windows host, builds it, runs the scripted race, and reports profiled
  stages, frame time, a paired A/B verdict (`ab`), the image check against the Xenos plugin
  (`oracle`, `diff`) or a demo clip. `lab pgr4 help` lists the verbs.
- `lab/` and `run/`: the PowerShell scripts those verbs run on the host: a FIFO queue and lock
  so only one game runs at a time, GUI launches in the desktop session, the input player and
  the St Petersburg route.
- `proton/`: the same race under Proton on Linux.
- `docs/`: how the renderer works, the measuring workflow, the perf plan, research notes, and
  setup for Windows (`docs/running-on-windows.md`) and Proton (`docs/proton.md`).
- `AGENTS.md`, `skills/pgr4-native/` and `prompts/`: what an agent reads first, the renderer
  performance rules taken from other Xbox 360 ports, and the briefs used to run a wave of
  agents with one integrator.

To use it you need [Bun](https://bun.sh) 1.1 or newer on the machine you drive from, and
[fleet](https://github.com/safzanpirani/fleet) with your Windows PC added as a host (OpenSSH
server, PowerShell 7). Then:

```sh
bun install
export RECOMP_HOST=<fleet host name of the Windows PC>
bun tools/lab/cli.ts pgr4 help
bun tools/lab/cli.ts pgr4 scripts      # install lab/ and run/ on the host
```

The scripts assume `C:\recomp` on the host (`builds\pgr4`, `logs`, `locks`); a junction works if
C: is small. Give each build tree an inbound firewall rule before its first run, or the "allow
access" prompt steals focus from the input player
(`New-NetFirewallRule -Direction Inbound -Action Block -Program <tree>\run\runtime\pgr4_recompiled.exe -DisplayName pgr4-<Name>`).
Stay logged in to the desktop and run as that user: games start in the desktop session through
a scheduled task. The host also needs Python 3 with numpy and Pillow, and ffmpeg with NVENC for
demo clips.

## About this repository

The code, tooling and docs are developed in a private repository and published here
automatically. History starts at the first public commit. Code comments and docs sometimes
point to development notes that are not published.

License: MIT ([LICENSE](LICENSE)). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
