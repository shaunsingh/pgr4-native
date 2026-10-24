# Shared context (every worker gets this)

Fill in the `<...>` fields. Everything else is the contract that held up in practice; change it
only for a reason.

```markdown
# Goal
PGR4 native renderer perf/correctness wave on <repo> (base <sha>, pushed). Target: 1280x800 at
60 fps under Proton on a Steam Deck. Integrator = the parent session; it merges your branch.
Time box: commit, push your branch, write notes and report by **<deadline>** (check `date`).
Stop launching new game runs after <deadline - 10 min>.

# Read first (short)
- `AGENTS.md`, `docs/perf-workflow.md` (the `lab pgr4` CLI, queue, trees, numbers),
  `docs/pgr4-perf-plan.md` (your slice row), `docs/pgr4-native-architecture.md` as needed,
  `skills/pgr4-native/references/native-renderer-performance.md` (rules from other Xbox 360
  ports). Prior art before experiments: if UnleashedRecomp / MarathonRecomp / XenonRecomp /
  ReXGlue already solved your problem, read how and adopt it.

# Current numbers (<date>, <machine>, uncapped race, tap off)
- <latest merged change and its ab result with CI, oracle dB>
- <where the frame time goes: game thread %, render thread %, largest profiled stages>

# Contract (all agents)
- Own git worktree: `git worktree add ../<repo>-<name> -b <name> <base>`. Commit there;
  `git push -u origin <name>`. Never commit to the base branch; never merge.
- Own build tree on the host (given per task). Never create a new tree (the firewall prompt
  steals focus and stalls runs). Never rebuild `native-<RefTree>` (the shared reference exe =
  <base>).
- Always `RECOMP_AGENT=<YourName>` on `lab pgr4` calls. Run every `--wait` command as a
  background command (timeout 3600); the result arrives on its own. No sleep/poll loops.
- One game at a time via the FIFO queue; <N> agents share it, so expect waits. Budget about
  **<K> game runs each** (oracle, profile, compare and each ab run count; an `ab` with
  `--rounds 1` = 2 runs). Queue one run at a time. Unique tags prefixed with your name in
  lowercase (e.g. `rtfix-o1`).
- Speed decisions: `lab pgr4 ab <RefTree> <YourTree> --b-ref HEAD --rounds 1 --tag <name>-ab1 --wait`
  (A = the reference exe without a rebuild, B = your committed HEAD, built first). Use
  `--rounds 2` only if one round is inconclusive and time allows. Image check:
  `lab pgr4 oracle <YourTree> <tag> --wait`, pass = each capture >= 30 dB and within ~1 dB of
  <reference range>.
- Your tree's first build: `lab pgr4 build <YourTree> --sdk --wait` if it was last built before
  <last SDK change>, else plain `build`.
- Never kill a `lab pgr4` wrapper that may have launched the game; if a run dies,
  `lab pgr4 clean <YourTree>`.
- Notes: append your own `#### <YourName>` section at the END of `notes/STATE.md` and a
  `## <date> <YourName>` section at the end of `notes/worklog.md` (union-merged). Facts,
  numbers, tags, commits, what is left. Do not edit other sections, the README, the perf plan or
  the rules file (the integrator does).
- File ownership is listed per task. Need a change in another agent's file? Message that agent
  first with the exact change; keep it minimal.
- Nothing from the game executable or game data in git.
- Report at the end: branch + commits, what changed, measured results (tags, numbers, CI,
  oracle dB), what failed, what is left.
```
