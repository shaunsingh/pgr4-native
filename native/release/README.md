# PGR4 native renderer

Project Gotham Racing 4 (Title Update #2), recompiled for 64-bit Windows from the Xbox 360
executable, with a native Direct3D 12 renderer. It is tuned for the Steam Deck OLED: 1280x800
at 90 fps, MSAA and motion blur on. It runs on Windows and, through Proton, on SteamOS and Linux.
It is built with the ReXGlue SDK and fixes from Xenia Canary; its frame rate unlock and game
fixes follow those MTR-PGR4 (github.com/MissingThePixel/MTR-PGR4) found.

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

## Geometry Wars

The arcade cabinet in the garage starts `gw_recompiled.exe`, which must stay beside
`pgr4_recompiled.exe`. Its settings are in `gw_recompiled.toml`.

## Licenses

`LICENSE.txt` is this project's license. `LICENSES/` holds those of the ReXGlue SDK, Xenia,
PGR4-Recomp, MTR-PGR4 and the libraries built into the program, and the sources it was built
from (`SOURCE.txt`). Project Gotham Racing 4 and its content belong to their owners; these
licenses grant no rights to the game.
