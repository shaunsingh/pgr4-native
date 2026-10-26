# PGR4 native renderer

Project Gotham Racing 4 (Title Update #2), recompiled for 64-bit Windows from the Xbox 360
executable, with a native Direct3D 12 renderer. It is tuned for the Steam Deck OLED, at 1280x800
and 90 fps with MSAA and motion blur on. It runs on Windows and, through Proton, on SteamOS and
Linux. It is built with the ReXGlue SDK and fixes from Xenia Canary, on the work of
safzanpirani/pgr4-native (github.com/safzanpirani/pgr4-native).

This package holds no game data. You need your own copy of the game.

## What goes where

Put these beside `pgr4_recompiled.exe`:

    game/            the disc's files: default.xex, Game/, UI/ and the rest
    game/Content/    Title Update #2's package, and downloadable content if you have it

- **game/**: the files extracted from your PGR4 disc (an ISO extractor such as XBOX360 ISO
  Extract does this). `game/default.xex` must exist.
- **game/Content/**: the packages as they come from Xbox Live or off a console, unchanged:
  - Title Update #2 (version 0.0.2.1), `TU_16L61VP_0000004000000.00000000000G1`. The game runs
    only with this update, since the code was recompiled from it. Its SHA-256 is
    `9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8`.
  - DLC, the files named by a 42-digit hex ID (Vehicle Pack 1 is
    `638152C719C45657B6640991BA6375D88A9D437D4D`), if you have it.

  The first start installs each new package into `userdata/`.

If you have the update's two files extracted instead of its package, put them in a folder named
`title-update/` beside the executable. That folder takes precedence over the package. Their
SHA-256:

    default.xexp         7e7c672b542dc20d20bed72e27603335701a6eb9f243d2a8a2468bb966ee9401
    Pgr4TitleUpdate.bin  f4bc47545df5fe8d0301bd4593d3b7b6e3871520f98e5f7249921973781a4b74

The first start creates `userdata/` (your save and the shader caches) and `logs/` beside the
executable. Keep `userdata/` when you update to a new release.

## Steam Deck

1. In Desktop Mode, copy this folder (with game/ filled in) anywhere, for example
   `~/Games/PGR4`.
2. Steam > Games > Add a Non-Steam Game to My Library > Browse, select `pgr4_recompiled.exe`.
3. In the shortcut's Properties > Compatibility, force a Proton version (tested: GE-Proton 11-5).
4. In Properties > General > Launch Options, enter `PROTON_USE_XALIA=0 DXVK_HDR=1 %command%`.
   Xalia, Proton's gamepad helper for desktop programs, otherwise takes about a tenth of a CPU
   core during races. `DXVK_HDR=1` lets the game output HDR where the display takes it (the
   OLED model, or an HDR TV, with HDR on in Steam's display settings).
5. Play it from Game Mode. The frame rate follows the display: 90 fps on the OLED, 60 on the
   LCD model. Steam's frame limiter works too.

## Windows

Run `pgr4_recompiled.exe`. It needs a GPU with Direct3D 12. Set `window_width` and
`window_height` in `pgr4_recompiled.toml` to your display's resolution. With HDR on in
Windows' display settings the game outputs HDR.

## Settings

`pgr4_recompiled.toml` beside the executable holds the settings, one per line. A flag on the
command line overrides it: in Steam's launch options, `%command% --fullscreen=false` for one.

## Upscaling

The release settings give the 3D scene 4x MSAA (`pgr4_native_msaa`: 1, 2 for the game's own, 4
or 8). More samples cost GPU time: on a Steam Deck OLED at 90 Hz in the St Petersburg race, with
16x anisotropic filtering, 4x averages 89.7 fps with 1% lows of 62.6 fps (GPU 86 % busy), and 8x
about 88 fps with 1% lows near 47 fps. With
`pgr4_native_upscaler = true` in `pgr4_recompiled.toml`, AMD's FidelityFX Super Resolution
antialiases the scene instead and upscales it to 1280x720. The scene renders at
`pgr4_native_upscaler_width` x `pgr4_native_upscaler_height`:

| Size | FSR's name for it |
|---|---|
| 1280x720 (default) | Native AA: antialiasing only |
| 853x480 | Quality |
| 752x423 | Balanced |
| 640x360 | Performance |

F7 switches between MSAA and the upscaler while you play, and F8 shows the upscaler's debug view.
Both start off. The interface always stays at full resolution, and no sharpening is added.

On a Steam Deck OLED, MSAA is still the fastest at 90 fps. In a St Petersburg race, MSAA averaged
89.6 fps, FSR at 1280x720 84.0 and FSR at 853x480 86.9. The upscaler is there for its image, and
for FSR 4.

Out of the box this is FSR 3.1, from AMD's signed `amd_fidelityfx_loader_dx12.dll` and
`amd_fidelityfx_upscaler_dx12.dll` beside the executable. The game calls only AMD's FidelityFX
API, so the tools below can put a newer upscaler behind it. They are not part of this package,
and none of them is tested with PGR4 yet. To see which upscaler runs, start the game once with
`%command% --log_level=info` and look for `PGR4 native upscaler:` in `logs/`: it names the
upscaler's version (`3.1.5` for the packaged FSR 3.1) and the render size.

### FSR 4 on a Steam Deck (Proton)

AMD's FSR 4.1.2 has a Lite model for handheld GPUs. Under Proton it falls back to FSR 3 because
it asks the Windows driver for the GPU family, and Wine has no answer.
[FSR4Injector](https://github.com/Rayekkk/FSR4Injector)'s `amdxc64.dll` supplies the answer.
Its Decky plugin sets up only Steam games, so for this one do it by hand:

1. Put two files beside `pgr4_recompiled.exe`:
   - `amdxc64.dll` from FSR4Injector's release (its `bin/`, LGPL).
   - AMD's `amdxcffx64.dll`, FidelityFX 2.4.0.4370 (FSR 4.1.2), from AMD's Adrenalin 26.10.1
     driver package. FSR4Injector's plugin downloads it from AMD into the games it sets up.
2. Set the launch options to
   `WINEDLLOVERRIDES=amdxc64=n,b FSR4I_FAMILY=150 FSR4I_REV=1 PROTON_USE_XALIA=0 DXVK_HDR=1 %command%`.
   The shim knows only the Legion Go 2's GPU by itself. Family 150 with revision 1 is the answer
   for which AMD's library picks the Lite model (FSR4Injector's `shim/src/fsr4i.c`), and the one
   OptiScaler gives for Lite. Add `FSR_WATERMARK=1` to have AMD's library show the model it runs.
3. Set `pgr4_native_upscaler = true`.

FSR4Injector lists the Steam Deck as untested.

### OptiScaler

[OptiScaler](https://github.com/optiscaler/OptiScaler) hooks the same FidelityFX calls and can
put FSR 4, XeSS or DLSS behind them.

- **Steam Deck, with fgmod** (Decky Framegen): set the launch options to
  `PROTON_USE_XALIA=0 DXVK_HDR=1 ~/fgmod/fgmod %command%`. fgmod copies OptiScaler in as
  `dxgi.dll` and replaces the package's `amd_fidelityfx_upscaler_dx12.dll` with its own, keeping
  the original as `amd_fidelityfx_upscaler_dx12.dll.b`. fgmod 0.7.9 brings OptiScaler 0.7.9 and
  AMD's FSR 4.0.2 upscaler, neither of which has the Lite model.
- **Windows**: put `OptiScaler.dll`, renamed to `dxgi.dll`, and `OptiScaler.ini` beside the
  executable.

In `OptiScaler.ini`, `Dx12Upscaler` picks the upscaler (`ffx` for FSR 3.1 and 4, `xess`,
`dlss`). For FSR 4's Lite model, put AMD's `amdxcffx64.dll` 4.1.2 beside the executable and use
an OptiScaler that knows the Lite model (its current source does) with `Fsr4GpuSupport=4`.
OptiScaler then answers AMD's GPU query itself, without FSR4Injector's shim. Keep the render
size in `pgr4_native_upscaler_width` and `_height`: the game renders at that size whatever
OptiScaler's own ratio settings say. Insert opens OptiScaler's menu (`ShortcutKey` in
`OptiScaler.ini` changes the key).

## Geometry Wars

The arcade cabinet in the garage starts `gw_recompiled.exe`, which must stay beside
`pgr4_recompiled.exe`. Its settings are in `gw_recompiled.toml`.

## Licenses

`LICENSE.txt` is this project's license. `LICENSES/` holds those of the ReXGlue SDK, Xenia,
PGR4-Recomp, MTR-PGR4 and the libraries built into the program, and the sources it was built
from (`SOURCE.txt`). Project Gotham Racing 4 and its content belong to their owners; these
licenses grant no rights to the game.
