# Changes

What each release changes for players, newest first. The release workflow publishes the section of
the tag's version above the player README (`.github/workflows/release.yml`), so a version gets its
section before it is tagged; changes land under Unreleased until then.

## Unreleased

**Picture**
- At 16:10 the scene and the UI's 3D models get the frame's aspect ratio, so the scene is no longer
  stretched 11 % taller and the medals keep their shape. The Options list no longer shows a row
  below its frame.
- The menus keep the game's screen in the 16:10 frame: the arcade's medals, the music player, the
  fades and the menu frames and washes reach the frame's edges.

**Upscaling and frame generation**
- Copies of one mesh along the track (barrier sections, chevron boards, kerbs) get their own motion,
  not a neighbour's, so FSR and frame generation no longer smear them when one comes into view.
- A mesh new to a frame (coming into view, or a level of detail changing) moves with the camera
  instead of standing still, so FSR no longer smears a block of road or building where it stood.

**Settings**
- The Options menu's Display, Driving and Developer pages save into `pgr4_recompiled.toml`, the
  file you edit, instead of a second file read over it, so an edit to it is no longer hidden by an
  older menu choice. Only the options you change are written, and the file's comments stay.
  Settings saved by 0.3.0 move into it at the first start.

**Debugging**
- The marks (F12) of an HDR session show the picture as it was, not rainbow bands.

## v0.3.0

**Picture**
- The game now renders at 1280x800, the Steam Deck panel's own 16:10 shape, not letterboxed 1280x720. The HUD sits at the top and bottom of the taller frame, and the dials keep their shape.
- The classic renderer (Xenos plugin, F9) draws the game's own 16:9 frame, so it no longer shows garbage below the scene at 16:10.
- No more magenta and black patches in the sky during lightning: colors are clamped to the render target's range, as the Xbox 360's EDRAM does.
- The light flares fade over 100 ms instead of a few frames, so they stop blinking when a light goes behind a car.
- The motion blur follows the sustained frame rate, so a paused frame no longer pumps (#1).
- Fixed the stretched triangles that appeared after hours of play (a race in the SDK's write tracking).

**Settings**
- The port's own settings are in the game's Options menu and in the pause menu, on three pages:
  - Display: renderer, aspect ratio, HDR, antialiasing (MSAA 2x to 8x or FSR), FSR resolution, dynamic resolution, frame generation, texture filtering, frame rate, frame latency.
  - Driving: the automatic gearbox and the cars' lights.
  - Developer: debug keys and diagnostics.
- New frame rate setting: the display's refresh rate, or a half or a third of it (`pgr4_native_present_interval`).
- A flag on the command line no longer gets saved into the settings file.

**Driving**
- F5 switches every car's lights on or off. The Driving page sets this for every race (`pgr4_headlights`).
- The automatic gearbox shifts down sooner and keeps the engine near the top of its range. The Driving page restores the original behaviour (`pgr4_automatic_keep_revs`).

**Upscaling and frame generation**
- New: FidelityFX Frame Generation (`pgr4_native_frame_generation`). The game runs at 45 fps and the display shows 90. The HUD is drawn into every frame, never interpolated.
- New: dynamic resolution (`pgr4_native_upscaler_target_fps`). The upscaler lowers its render size when the GPU falls behind, and learns how much each size change saves.
- FSR's image is better:
  - It gets the scene's real camera.
  - It gets motion for the sky.
  - Its motion vectors stay bounded and finite.
  - Its color is read in the EDRAM's range.
  - Alpha-to-coverage dither (fences) no longer leaves a screen door.
  - The motion blur's mask is kept, so the player's car stays sharp.
- The upscaler sizes now come from the 1280x800 frame. The default is 1280x800, antialiasing only.
- One debug switch (F8) for FSR and frame generation, with AMD's watermark for FSR 4.
- F7 and F8 work in every session.

**Performance and stability**
- The renderer's Direct3D 12 calls now run on a thread of their own. In the Tokyo race in the rain, the render thread's time on draws went from 6.6 to 4.1 ms a frame. Its slowest frames went from 10 to 5.6 ms, which leaves the GPU as the limit.
- Motion vectors are cheaper to compute: each draw finds its previous-frame match in order.
- The upscaled scene's alpha comes in the resolve copy, which saves a full-screen pass (about 0.15 to 0.2 ms of GPU time).
- The pipeline cache keeps what you actually play, so prewarmed pipelines no longer age out.
- Fixed a crash at about one start in four with OptiScaler: `d3d12.dll` now loads at its first call.
