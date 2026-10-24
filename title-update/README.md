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

`native/build.sh --update <folder>` names the folder that holds them; the default is
`~/recomp/dumps/pgr4/title-update`. They
come unchanged from the package `TU_16L61VP_0000004000000.00000000000G1` (LIVE, content type
0x000B0000, title 4D5307F9; SHA-256
`9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8`); every 4 KB block of each
is in it. The package also patches Geometry Wars (`gw.xexp`); that file is left out, since the
Geometry Wars code here is generated from the disc's `gw.xex` and the runtime would patch the
image under it.

| File | SHA-256 |
|---|---|
| `default.xexp` | `7e7c672b542dc20d20bed72e27603335701a6eb9f243d2a8a2468bb966ee9401` |
| `Pgr4TitleUpdate.bin` | `f4bc47545df5fe8d0301bd4593d3b7b6e3871520f98e5f7249921973781a4b74` |

At run time the install's `runtime/title-update/` holds the two game files. The runtime mounts
that folder as `update:\` when no `--update_data_root` is given, applies `update:\default.xexp`
to `game:\default.xex`, and refuses to start when the image it loaded is not the one the code
was generated from (`sdk/patches/0026` and `0028`).

## Downloadable content

DLC is the player's own, copied off a console as STFS packages (the files named by a 42-digit hex
ID, such as Vehicle Pack 1's `638152C719C45657B6640991BA6375D88A9D437D4D`). Put them in the game
folder's `Content\` (or point `--content_root` at their folder). At startup the runtime installs
each of this title's marketplace packages it has not installed yet into the user data's content
store, where the game finds it; the next start skips it.
