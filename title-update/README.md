# Title Update #2

The build recompiles PGR4 as Title Update #2 (version 0.0.2.1, built 2008-02-01) rather than
the disc's 0.0.0.1: it is the last version of the game, it adds World Challenge mode, and the
game reads downloadable content such as Vehicle Pack 1 only with it. Like the disc and any DLC,
the update's files are the player's own: this folder holds only what the build derives for it.

| File | What |
|---|---|
| `game-files.sha256` | The SHA-256 of the update's two game files, which the build checks |

The codegen entries for 0.0.2.1 (setjmp/longjmp, function starts) are in
`game/pgr4/pgr4recompiled_manifest.toml`. The base-to-0.0.2.1 address map of the former MTR-PGR4
workflow is in `retired/title-update/`.

The two game files, which this repository does not contain:

| File | What |
|---|---|
| `default.xexp` | The delta patch that turns the disc's `default.xex` into 0.0.2.1. Codegen applies it to a copy of the disc's file beside it, the runtime finds it on `update:\` |
| `Pgr4TitleUpdate.bin` | The update's own data, read by the game from `update:\` |

They come unchanged from the package `TU_16L61VP_0000004000000.00000000000G1` (LIVE, content
type 0x000B0000, title 4D5307F9; SHA-256
`9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8`). `native/build.sh --update`
takes either the package, which it extracts into `<dest>/title-update` with the SDK's
`rexglue vfs-dump` (`sdk/patches/0034`), or a folder that holds the two files; the default is
the folder `~/recomp/dumps/pgr4/title-update`. The package also patches Geometry Wars
(`gw.xexp`). That patch is never applied, since the Geometry Wars code here is generated from
the disc's `gw.xex`: the build leaves the file out, and `gw_recompiled.exe` turns the SDK's
`apply_title_update` off.

| File | SHA-256 |
|---|---|
| `default.xexp` | `7e7c672b542dc20d20bed72e27603335701a6eb9f243d2a8a2468bb966ee9401` |
| `Pgr4TitleUpdate.bin` | `f4bc47545df5fe8d0301bd4593d3b7b6e3871520f98e5f7249921973781a4b74` |

At run time the runtime finds the update in one of two forms, as Xenia Canary does for the
first:

- **The package** in the game folder's `Content\` (or the folder `--content_root` names), beside
  any DLC. At startup, before the executable loads, the runtime installs each package there it
  has not installed yet into the user data's content store, under the title and content type
  the package's header names. When it loads `default.xex`, it mounts the installed title update
  as `update:\` and applies its `default.xexp` (`sdk/patches/0029` and `0033`).
- **The two files extracted** into `title-update/` beside the executable, or the folder
  `--update_data_root` names. The runtime mounts that folder as `update:\` (`0026` and `0027`).
  It takes precedence over an installed package. The dev install (`run/runtime/title-update/`)
  uses this form.

Either way the game reads `update:\Pgr4TitleUpdate.bin`, and the runtime refuses to start when
the image it loaded is not the one the code was generated from (`0028`).

## Downloadable content

DLC is the player's own, copied off a console as STFS packages (the files named by a 42-digit hex
ID, such as Vehicle Pack 1's `638152C719C45657B6640991BA6375D88A9D437D4D`). Put them in the game
folder's `Content\`, beside the title update's package. The runtime installs them in the same
pass into the content store, where the game finds them; the next start skips them.
