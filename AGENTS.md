<coding_guidelines>
# Working in pgr4-native

This repository holds a native D3D12 renderer for the PGR4 static recompilation, the game
projects and ReXGlue patches it builds with, and the scripts that build, run and measure it on a
Steam Deck. The build cross-compiles for Windows on Linux and the result runs under Proton. Game
data and builds stay outside the repository (`~/recomp/dumps/pgr4`, `~/recomp/builds/pgr4`).

## Start of every session

1. Read `README.md` (status, numbers, bugs) and `docs/pgr4-native-architecture.md` (how the
   renderer works).
2. Read the header comments of `native/build.sh` and `proton/deck-run.sh` before building or
   running anything.
3. Check that no game runs and nobody is playing (`pgrep -l 'pgr4_recomp|deck-run|gamescope-wl'`
   and the screen) before a run or a heavy build.

## Rules

- **No game data in git.** Discs, executables, title updates, extracted assets, saves, captures
  of game imagery, and C++ generated from the game's executable stay out of the repository.
  `.gitignore` blocks the common extensions; check `git status` before every commit anyway.
- **Never use leaked or copyrighted source code.** Work from your own disc and public tools.
- **Run it to prove it.** A build that compiles is not evidence. Evidence is the game running:
  a screenshot, a frame-time log, an `ab_compare.py` verdict, an oracle PSNR. Put the numbers,
  tags and paths in your notes.
- **Prior art before experiments.** For a problem other open-source ports have probably solved
  (renderer performance, API traps, frame pacing), read their code first, adopt the technique,
  then confirm it with one measured run. Renderer rules with sources:
  `skills/pgr4-native/references/native-renderer-performance.md`.
- **Upstream first.** SDK changes are patches in `sdk/patches/` on the ReXGlue commit
  `sdk/rexglue.lock` names. Take fixes from ReXGlue `development` or Xenia Canary where they
  exist and say so in the patch header; game-specific behaviour belongs in `game/<title>/`.
- **Licenses.** Code from projects without a license (FM2-ReGPU, for one) is reference only.
  `docs/research/` describes such code with file and line numbers and copies none of it. Ported
  Xenia/ReXGlue code keeps its BSD-3-Clause notice (`THIRD_PARTY_NOTICES.md`).
- **One game at a time.** The run scripts refuse to start beside a running game. Perf runs need
  the Deck otherwise idle. Do not kill processes you did not start.
- **Upstream contributions:** check each upstream's policy on AI-generated code before opening
  a PR.

## Several agents

- One git worktree per agent (`git worktree add ../pgr4-native-<name> -b <name>`) and one build
  tree per agent (`native/build.sh --dest ~/recomp/builds/pgr4/native-<name>`), merged by one
  integrator.
- Each agent owns named files; changes to someone else's file go through that agent first.

## Layout

| Path | What |
|---|---|
| `native/` | The renderer (`native/src`), `build.sh`, the release files (`native/release/`), Python tools (`native/tools/`) |
| `game/` | The game projects: `pgr4/` and `gw/` (manifest, CMake, app), `common/` (title handoff) |
| `sdk/` | `rexglue.lock` (the ReXGlue commit) and `patches/`, applied in order |
| `build/` | The xwin splat (`fetch-xwin.sh`) and the clang-cl toolchain file |
| `proton/` | Runs under Proton: `deck-run.sh` (measured race), `play.sh` (play, Steam shortcut), `route_player.py` |
| `run/` | The scripted routes and their draw counts |
| `title-update/` | The SHA-256 of Title Update #2's files the build checks |
| `docs/` | Architecture, design, plans, research notes |
| `skills/pgr4-native/` | Agent skill and the renderer performance rules |
| `retired/` | The former Windows workflow on MTR-PGR4, kept for the record (`retired/README.md`) |
</coding_guidelines>
