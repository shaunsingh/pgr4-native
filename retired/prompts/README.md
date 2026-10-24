# Prompts for multi-agent sessions

These are the briefs used to run several coding agents on the renderer at once: one integrator
session plus three or four worker agents, each in its own git worktree and its own build tree,
sharing one Windows build host through the run queue. They were written for a harness whose
`task` tool spawns worker agents with a shared `context` and a per-agent `task`, and whose agents
can message each other (`write agent://<Name>`) and run commands in the background. Adapt the
mechanics to your harness; the rules carry over.

| File | What |
|---|---|
| `context.md` | The shared header every worker gets: goal, read-first list, current numbers, the contract |
| `agent-brief.md` | The per-agent task template: slice, worktree, build tree, files, queue budget, acceptance, notes, report |
| `integrator.md` | What the integrator does before, during and after a wave |
| `examples.md` | Real briefs and tables from earlier waves, with names of machines and people removed |

## How a session runs

1. The integrator measures a baseline on the current base commit, picks the slices
   (`docs/pgr4-perf-plan.md`), and assigns each agent a slice, a build tree that already has a
   firewall rule, and the files it owns. A structural change (a new thread, a moved module)
   conflicts with every small optimization in the same files, so the integrator decides up front
   whether it merges first or last.
2. The integrator keeps one tree as the shared reference (built from the base commit, never
   rebuilt during the wave), so every agent's `lab pgr4 ab` has the same A side.
3. Workers start with `context.md` plus their own `agent-brief.md`. Each builds its tree, takes
   its own baseline if the brief asks for one, changes code, and settles every claim with
   `lab pgr4 ab` and `lab pgr4 oracle`. Runs queue: one game at a time, 4-5 minutes each, and
   with four agents a run can wait 5-25 minutes.
4. Workers commit and push their branch, append their sections to `notes/STATE.md` and
   `notes/worklog.md`, and report.
5. The integrator merges in the agreed order, runs `lab pgr4 check <tree> <prefix> --wait` on
   the merged code, updates the README numbers, and removes the worktrees.

The queue sets the pace. Four agents with about four game runs each make 16 runs of 4-5 minutes,
over an hour of game time on one host, so give each agent a run budget and a deadline.
