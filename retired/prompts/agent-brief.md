# Per-agent brief (template)

One brief per worker, sent with `context.md`. Keep it to what this agent needs: the slice, where
it works, what it owns, how it proves the result. Fields in `<...>`.

```markdown
# Target
<Slice # and name from docs/pgr4-perf-plan.md>: <the hypothesis in one or two sentences, with
the number that motivates it, e.g. "`track` was 251 ns/draw (~0.8 ms/frame at 3,300 draws);
the game thread is the bottleneck. Cut it.">
Build tree: `native-<Tree>` (<old: first build `lab pgr4 build <Tree> --sdk --wait` | current>).
Files you own: <paths>. <Agent X> may need small edits in <file>; accept them via messages and
keep its additions intact.

# Change
- Baseline first, in your tree: `lab pgr4 profile <Tree> <name>-base --ref <base> --wait`
  (rerun if it reports compiler processes). Read where the time goes before changing code.
- <What to try, in order of expected payoff. Say what must stay identical: same draws, same
  state, same image.>
- Then `lab pgr4 profile <Tree> <name>-opt1 --wait` and one `ab` round against <RefTree>
  (`--tag <name>-ab1`).

# Acceptance
- Stage numbers before/after (ns/draw), the `ab` verdict with its 95 % interval, oracle >= 30 dB
  on your final HEAD (`<name>-o1`) if the run budget allows.
- Branch `<name>` pushed, notes sections written, report.
```

## Checklist for the integrator writing it

- **Name and slice.** A short CamelCase name (it becomes the branch, the tag prefix and the
  queue owner) and the perf-plan row it works on.
- **Worktree.** Created from the wave's base commit; removed with `git worktree remove --force`
  after the merge.
- **Build tree.** One with a firewall rule (`lab pgr4 trees`), not used by another agent and not
  the reference tree. Say whether its first build needs `--sdk`.
- **Files owned and files to leave alone.** Name the shared files and who may edit them. Two
  agents in one file is fine when one only adds a registration line; say which.
- **Queue budget.** Count runs: profile 1, oracle 1, compare 1 per mode, `ab --rounds 1` 2. With
  four agents, about four runs each. One queued run at a time. A deadline in wall-clock time,
  and a time to stop queueing new runs.
- **Acceptance.** `lab pgr4 ab` with B faster and the 95 % interval excluding zero (`--rounds 2`
  for changes under ~3 %). Oracle >= 30 dB on each of the 3 captures and within ~1 dB of the
  agent's own baseline; `lab pgr4 diff <tag>` if not. No new hitches (`perf_hitch.py`).
- **Notes.** `#### <Name>` at the end of `notes/STATE.md` (results with tags, stopping point,
  exact next commands) and `## <date> <Name>` at the end of `notes/worklog.md`.
- **Report.** Files changed, numbers with run tags, what failed, what is left, commands to
  resume. No build or game run outside the queue.
