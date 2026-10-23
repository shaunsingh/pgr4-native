# Title Update #2

The build recompiles PGR4 as Title Update #2 (version 0.0.2.1, built 2008-02-01) rather than
the disc's 0.0.0.1: it is the last version of the game, it adds World Challenge mode, and the
game reads downloadable content such as Vehicle Pack 1 only with it. Microsoft distributed it
free through Xbox Live, so it ships here; the disc and any DLC stay the player's own.

| File | What |
|---|---|
| `default.xexp` | The delta patch that turns the disc's `default.xex` into 0.0.2.1. Codegen applies it to a copy of the disc's file beside it (`build.sh`/`build.ps1`), the runtime finds it on `update:\` |
| `Pgr4TitleUpdate.bin` | The update's own data, read by the game from `update:\` |
| `entrypoint.toml` | setjmp/longjmp and the function starts codegen needs for 0.0.2.1, in place of MTR-PGR4's |
| `addresses.txt` | Base-to-0.0.2.1 addresses of MTR-PGR4's generated-code patches and manifest entries |
| `apply_game_patches.patch` | The one MTR-PGR4 generated-code patch 0.0.2.1 needs rewritten beyond its addresses: the garage walking step count, which the update keeps in r29 |

The two game files come from the package `TU_16L61VP_0000004000000.00000000000G1` (LIVE, content
type 0x000B0000, title 4D5307F9; SHA-256 `9980cf82c3bbbb32b35c841532c832b5598e99ce50daba8c658645213ab3d9b8`) unchanged; every 4 KB block of
each is in it. The package also patches Geometry Wars (`gw.xexp`); that file is left out, since
the Geometry Wars code here is generated from the disc's `gw.xex` and the runtime would patch
the image under it.

| File | SHA-256 |
|---|---|
| `default.xexp` | `7e7c672b542dc20d20bed72e27603335701a6eb9f243d2a8a2468bb966ee9401` |
| `Pgr4TitleUpdate.bin` | `f4bc47545df5fe8d0301bd4593d3b7b6e3871520f98e5f7249921973781a4b74` |

At run time the install's `runtime/title-update/` holds the two game files. The runtime mounts
that folder as `update:\` when no `--update_data_root` is given, applies `update:\default.xexp`
to `game:\default.xex`, and refuses to start when the image it loaded is not the one the code
was generated from (`native/rexglue-content.patch`).

## Downloadable content

DLC is the player's own, copied off a console as STFS packages (the files named by a 42-digit hex
ID, such as Vehicle Pack 1's `638152C719C45657B6640991BA6375D88A9D437D4D`). Put them in the game
folder's `Content\` (or point `--content_root` at their folder). At startup the runtime installs
each of this title's marketplace packages it has not installed yet into the user data's content
store, where the game finds it; the next start skips it.
