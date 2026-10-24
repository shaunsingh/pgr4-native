---
name: pgr4-native
description: Use when building, running, profiling or image-checking the PGR4 native D3D12 renderer on a Steam Deck, patching its ReXGlue SDK, or optimizing a native renderer for an Xbox 360 static recompilation.
---

# pgr4-native

Read the repository's `AGENTS.md` first. The build cross-compiles for Windows on Linux
(`native/build.sh`, in the `pgr4dev` distrobox) and the game runs under Proton in gamescope.

```sh
native/build.sh [--stage sdk,game,install] [--dest DIR]       # pinned ReXGlue + sdk/patches, game/
proton/deck-run.sh deck <tag> --refresh 90 --hold 45 --output eDP-1   # measured race, 1 % lows
proton/deck-run.sh deck <tag> --mode oracle --hold 20          # image PSNR against Xenos
proton/deck-run.sh <unpacked release> <tag> --mode release      # a release as players start it
python3 native/tools/ab_compare.py <log A> <log B>              # is B faster? 95 % interval
proton/play.sh [--install]                                      # play; Steam shortcut
```

A run takes about 4 minutes; start it in the background and wait for it to end.

## Rules

- Prior art before experiments. Many Xbox 360 ports are open source. For a problem they have
  probably solved (renderer performance, API traps, frame pacing), read their code first,
  adopt the technique, then confirm it with one measured run.
- No game data, title updates, captures of game imagery or code generated from the game's
  executable in git.
- Evidence is the game running (frame times, an `ab_compare.py` verdict, oracle dB, a
  screenshot), written into your notes with run tags.
- Measure with release flags (tap and traces off) on an otherwise idle Deck.
- Acceptance for a change: `ab_compare.py` says B is faster with the 95 % interval excluding
  zero, the 1 % low does not drop, and the oracle stays within about 1 dB of your own baseline
  on each of its 3 captures (45-48 dB on the St Petersburg race).
- SDK changes go into `sdk/patches/` on the commit `sdk/rexglue.lock` pins, taken from ReXGlue
  `development` or Xenia Canary where they exist.
- How the renderer works: `docs/pgr4-native-architecture.md`. What to work on next: the
  README's "What's next".
- Native renderer performance (any project): read `references/native-renderer-performance.md`
  first. It holds 17 rules taken from UnleashedRecomp, MarathonRecomp, Xerenge and other Xbox
  360 ports, each with its source and PGR4's status. The ones that matter most: measure with
  release flags and report CPU and GPU ms; replace the guest D3D API instead of wrapping it;
  turn on the recompiler's register-as-local codegen options; translate shaders offline and
  compile pipelines before play; use bindless resources, dirty-flag flushes and deferred
  resolve copies; after `SetPipelineState`, set dynamic depth bias again.
