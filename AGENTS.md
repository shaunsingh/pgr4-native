# Working in pgr4-native

This repository holds a native D3D12 renderer for the PGR4 static recompilation, the game
projects and ReXGlue patches it builds with, and the scripts that build, run and measure it on a
Steam Deck. The build cross-compiles for Windows on Linux, and the result runs under Proton.

Before building or running anything, read [docs/architecture.md](docs/architecture.md) for how
the renderer works and [docs/development.md](docs/development.md) for the build, debugging,
measurement, SDK patch and performance rules. Then read the header comments of
`scripts/build.sh` and `scripts/deck-run.sh`.

- Keep game data out of git (discs, executables, title updates, assets, saves, captures of game
  imagery, generated C++), and check `git status` before every commit.
- Run one game at a time, and only when nobody is playing. Check
  `pgrep -l 'pgr4_recomp|deck-run|gamescope-wl'` and the screen before a run or a heavy build,
  and never kill processes you did not start.
- Prove a change by running the game, not by a build that compiles.
- Look for prior art first in ReXGlue `development`, Xenia Canary and other ports, and name the
  source in the patch or comment that adopts it. SDK changes are patches in `patches/`, and
  game-specific behaviour goes in `games/<title>/`.
- Format touched C++ with the root `.clang-format`.
- Give each agent its own git worktree and build tree (`scripts/build.sh --dest`).
- Check each upstream's policy on AI-generated code before opening a pull request there.
