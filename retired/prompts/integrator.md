# Integrator checklist

The integrator is the session that spawns the workers and merges their branches. It does not
take a slice that needs many game runs; the queue is already full.

## Before the wave

1. Pull, then check the host: `lab pgr4 status` (empty queue, free lock, no compilers) and
   `lab pgr4 trees` (which trees have a firewall rule).
2. Install the current host scripts: `lab pgr4 scripts`.
3. Build the reference tree from the base commit (`lab pgr4 build <RefTree> --ref <base> --wait`)
   and leave it alone for the whole wave. Every agent's `ab` uses it as the A side.
4. Pick the slices and assign trees and file ownership. Decide the merge order now. A structural
   refactor either merges first (the others rebase onto it) or last (its own agent rebases and
   resolves); tell that agent which.
5. Spawn the workers with `context.md` and one `agent-brief.md` each, in one batch.

## During the wave

- Broadcast tool changes to all agents (a fixed script, a new `lab pgr4` flag). Fixes to shared
  scripts go into `lab/` or `run/` and `lab pgr4 scripts`, never only into the installed copy.
- Answer ownership questions quickly; an agent waiting on a message is not using its run budget.
- Watch `lab pgr4 status` for a stuck lock. A wrapper that died leaves the game, the input task
  and the lock behind: the owning agent runs `lab pgr4 clean <tree>`.

## After the wave

1. Merge each branch in the agreed order: `git merge <branch>`. The worklog merges by union.
   For notes-file tail conflicts, where both sides appended a section, run
   `tools/resolve-append-conflicts.py notes/STATE.md` (needs `git config merge.conflictStyle
   diff3`), then `git add` and commit. Real conflicts stay in place for you to resolve.
2. Run the integrator check on the merged code:
   `lab pgr4 check <IntegratorTree> <prefix> --wait` (build, cold and warm profiled runs, oracle,
   primary and Xenos compare).
3. Update the README's status and numbers, the perf plan's slice rows, and your own notes
   section.
4. Remove the worktrees: `git worktree remove --force ../<repo>-<name>`.
