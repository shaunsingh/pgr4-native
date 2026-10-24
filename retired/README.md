# Retired

The Windows workflow this renderer was first built and measured with, kept for the record. None
of it is maintained, and the paths inside these files are the ones they had before they moved
here. It built on a checkout of MissingThePixel/MTR-PGR4 and the ReXGlue SDK v0.10.0 with MTR's
patch; the build now starts from ReXGlue itself (`sdk/rexglue.lock`, `sdk/patches/`) and the
game projects in `game/`, on Linux.

| Path | What it was | Replaced by |
|---|---|---|
| `native/build.ps1`, `native/run.ps1` | Build of the renderer into a copy of the MTR-PGR4 checkout, and its start/stop in the Windows desktop session | `native/build.sh`, `proton/deck-run.sh`, `proton/play.sh` |
| `native/rexglue-*.patch`, `instrument/rexglue-drawcount.patch` | Patches to the MTR-patched SDK v0.10.0 | `sdk/patches/`, on ReXGlue `development` |
| `native/xenos-texture-dump.patch`, `native/tools/texture_harness/` | Texture dumps from the Xenos plugin and a standalone untiling check of them, built with MSVC | The oracle (`deck-run.sh --mode oracle`) |
| `instrument/` | A draw-count build of MTR-PGR4 | `--gpu_stream_stats` in `sdk/patches/` |
| `build/build-mtr.ps1` | Build of MTR-PGR4 and its SDK | `native/build.sh --stage sdk` |
| `title-update/entrypoint.toml`, `apply_game_patches.patch`, `addresses.txt` | MTR's manifest entries and generated-code edits moved to Title Update #2's addresses | `game/pgr4/pgr4recompiled_manifest.toml` (`[entrypoint]` and its midasm hooks) |
| `lab/`, `tools/lab/`, `package.json`, `bun.lock`, `tsconfig.json`, `run/*.ps1` | The `lab pgr4` CLI and the PowerShell behind it: build trees, run queue, A/B, oracle and demo runs on a Windows PC driven over `fleet` | `proton/deck-run.sh` with `native/tools/` |
| `proton/run.sh`, `docs/proton.md` | A race under Proton on that PC's Linux install, reading the Windows build tree | `proton/deck-run.sh` |
| `docs/running-on-windows.md`, `docs/perf-workflow.md`, `prompts/`, `tools/resolve-append-conflicts.py` | Setup of the Windows host, its measuring workflow, and the briefs and merge helper for several agents on it | `AGENTS.md`, the README's Steam Deck section |
