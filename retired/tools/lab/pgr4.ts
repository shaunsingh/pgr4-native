/** `lab pgr4 ...`: build, run, profile and check the PGR4 native renderer on the Windows build host in one command
 *  each, instead of hand-made tar/fleet cp/fleet spawn/sleep/tail chains (docs/perf-workflow.md).
 *  Runs from any worktree: sources come from the git checkout of the current directory. */
import { mkdirSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { HOST, runScript } from "./main.ts";
import { PGR4, ROOT } from "./layout.ts";

const BUILDS = "C:\\recomp\\builds\\pgr4";
const SCRIPTS = `${BUILDS}\\lab-scripts\\perf`;
const RUN_SCRIPTS = `${BUILDS}\\lab-scripts\\run`;

export const PGR4_HELP = `lab pgr4 — the PGR4 native renderer on ${HOST} (docs/perf-workflow.md)

  <tree> is a build tree: CutOver, DrawPerf, ... (= ${BUILDS}\\native-<tree>) or a full path.
  Use only trees with a firewall rule (lab pgr4 trees). Sources come from the git checkout of
  the current directory (worktrees work), or from --ref <commit> via git archive.
  --wait blocks until the remote job ends and prints its tail: run it as an async/background
  command and the result arrives when the job is done (no sleep guessing).

  lab pgr4 scripts                   install lab/*.ps1 (+ rr_run.ps1) and native/tools/*.py into
                                     ${SCRIPTS}, run/*.ps1 and run/*.keys into
                                     ${RUN_SCRIPTS}, and parse-check them
  lab pgr4 sync <tree> [--ref R]     tar native+instrument and upload as <tree>\\lab\\perf-native.tar
  lab pgr4 build <tree> [--sdk] [--ref R] [--no-gate] [--wait]
                                     sync + build (--sdk: first build of an old tree)
  lab pgr4 profile <tree> <tag> [--extra "<flags>"] [--no-build] [--ref R] [--wait]
                                     sync + build + one profiled race run + stage/hitch tables
  lab pgr4 compare <tree> [--modes primary,none] [--extra "<flags>"] [--suffix s] [--wait]
                                     unprofiled race fps from the game log, one run per mode
  lab pgr4 oracle <tree> <tag> [--wait]
                                     race image oracle (PSNR vs Xenos) through the run queue
  lab pgr4 check <tree> <prefix> [--no-build] [--ref R] [--extra "<flags>"] [--wait]
                                     integrator check: build, cold+warm profiled runs, oracle,
                                     primary and Xenos compare
  lab pgr4 ab <treeA> [<treeB>] [--a-ref R] [--b-ref R] [--a-extra F] [--b-extra F]
          [--a-mode M] [--b-mode M] [--rounds 2] [--tag ab] [--wait]
                                     A/B bench: alternating unprofiled race runs, compared bin by
                                     bin after the race mark (paired, with a 95 % interval).
                                     One tree + two flag sets for a switch; two trees (+ --a-ref /
                                     --b-ref, built first) for two builds
  lab pgr4 wait <job> [--timeout S]  wait for a fleet job (<host>:<id> or <id>), print its tail
  lab pgr4 out <tree> <tag> [--all]  a run's <tree>\\lab\\<tag>.out as clean text
  lab pgr4 diff <tag|dir> [--tiles 16x9] [--gain 8]
                                     where native and Xenos differ in an oracle run's captures
                                     (C:\\recomp\\logs\\pgr4-<tag>-captures): side-by-side +
                                     heatmap PNGs and a worst-tile report, copied to
                                     .cache/pgr4/diff/<tag>/
  lab pgr4 demo <tree> [--xenos] [--seconds 20] [--delay 8] [--uncapped] [--extra F] [--tag demo] [--wait]
                                     record the race (game window, NVENC) from <delay> s after the
                                     race mark; --xenos adds a Xenos run at the same route time and
                                     a labelled side-by-side; stills at 25/50/75 %. --wait fetches
  lab pgr4 demo-get <tag>            copy sbs.mp4 (else native.mp4) and the stills to
                                     .cache/pgr4/demo/<tag>/
  lab pgr4 status                    queue, lock, running games, compilers, tasks, debug file
  lab pgr4 trees                     build trees, firewall rule, exe time
  lab pgr4 clean <tree> [--force]    after a dead wrapper: stop that tree's game, end the input
                                     task, remove the lock (refuses while another tree's game runs)

  --owner <name> (default $RECOMP_AGENT or "agent") labels queue tickets and the lock.`;

function treePath(t: string): string {
  return /[\\:]/.test(t) ? t : `${BUILDS}\\native-${t}`;
}

function flag(args: string[], name: string): string | undefined {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}

function positional(args: string[]): string[] {
  const out: string[] = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i]!;
    if (a.startsWith("--")) {
      if (["--ref", "--extra", "--modes", "--suffix", "--owner", "--timeout", "--tiles", "--gain", "--a-ref",
           "--b-ref", "--a-extra", "--b-extra", "--a-mode", "--b-mode", "--rounds", "--tag", "--seconds",
           "--delay"].includes(a)) i++;
      continue;
    }
    out.push(a);
  }
  return out;
}

// FLEET_WIN_SESSION=0: fleet's persistent Windows PowerShell session sometimes loses a command's
// output ("the Windows session lost this program's PowerShell output") and then reports a launch
// that did happen as failed. One-shot sessions avoid it.
const FLEET_ENV = { ...process.env, COPYFILE_DISABLE: "1", FLEET_WIN_SESSION: "0" };

async function run(cmd: string[], opts: { cwd?: string; quiet?: boolean } = {}): Promise<{ out: string; err: string; code: number }> {
  const proc = Bun.spawn(cmd, { cwd: opts.cwd, stdout: "pipe", stderr: "pipe", env: FLEET_ENV });
  const [out, err] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text()]);
  const code = await proc.exited;
  if (!opts.quiet && err) process.stderr.write(err);
  return { out, err, code };
}

async function sh(cmd: string[], opts: { cwd?: string; quiet?: boolean } = {}): Promise<string> {
  const { out, err, code } = await run(cmd, opts);
  if (code !== 0) throw new Error(`${cmd.join(" ")} exited ${code}${opts.quiet && err ? `: ${err.trim()}` : ""}`);
  return out;
}

async function checkout(): Promise<string> {
  try {
    return (await sh(["git", "rev-parse", "--show-toplevel"], { quiet: true })).trim();
  } catch {
    return ROOT;
  }
}

/** Uploads native + instrument (working tree, or `ref` via git archive) as <tree>\lab\perf-native.tar. */
export async function sync(tree: string, ref?: string): Promise<void> {
  const top = await checkout();
  const tmp = mkdtempSync(join(tmpdir(), "pgr4-sync-"));
  try {
    let base = join(top, PGR4);
    if (ref) {
      const arch = join(tmp, "src.tar");
      await sh(["git", "-C", top, "archive", "-o", arch, ref, join(PGR4, "native"), join(PGR4, "instrument")]);
      await sh(["tar", "-xf", arch, "-C", tmp]);
      base = join(tmp, PGR4);
    }
    const tar = join(tmp, "perf-native.tar");
    await sh(["tar", "--exclude", "__pycache__", "-cf", tar, "-C", base, "native", "instrument"]);
    const dest = treePath(tree);
    await sh(["fleet", "exec", HOST, `New-Item -ItemType Directory -Force '${dest}\\lab' | Out-Null`], { quiet: true });
    await sh(["fleet", "cp", tar, `${HOST}:${dest}\\lab\\perf-native.tar`], { quiet: true });
    const what = ref ? `${ref} (git archive)` : `${top} working tree`;
    console.log(`synced ${what} → ${dest}\\lab\\perf-native.tar`);
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
}

/** Starts a PowerShell command line as a fleet job on the host; returns "<host>:<id>". fleet can print a
 *  job id and still exit nonzero ("launch unconfirmed") when the job did start: then the job's log
 *  decides, so the caller never relaunches a run that is already going. */
async function spawn(commandLine: string): Promise<string> {
  const { out, err, code } = await run(["fleet", "spawn", HOST, commandLine], { quiet: true });
  const m = (out + err).match(new RegExp(`${HOST}:([\\w-]+)`));
  if (!m) throw new Error(`fleet spawn failed (exit ${code}): ${(out + err).trim()}`);
  const job = `${HOST}:${m[1]}`;
  if (code !== 0) {
    const log = await run(["fleet", "jobs", "log", job], { quiet: true });
    if (!log.out.trim()) throw new Error(`fleet spawn exited ${code} and ${job} has no log yet; inspect it with fleet jobs log ${job} before retrying`);
    console.log(`fleet reported exit ${code} but ${job} is running (its log has output)`);
  }
  console.log(`job ${job}`);
  return job;
}

export async function waitJob(job: string, timeout?: string, lines = 120): Promise<void> {
  const id = job.includes(":") ? job : `${HOST}:${job}`;
  const args = ["fleet", "jobs", "wait", id];
  if (timeout) args.push("--timeout", timeout);
  const proc = Bun.spawn(args, { stdout: "ignore", stderr: "inherit", env: FLEET_ENV });
  const code = await proc.exited;
  const tail = await sh(["fleet", "jobs", "tail", id, "-n", String(lines), "--exclude", "slow upload|Building CXX"], { quiet: true });
  process.stdout.write(tail);
  if (code !== 0) throw new Error(`job ${id} ended with exit ${code}`);
}

function ps(script: string, params: Record<string, string | undefined>, switches: Record<string, boolean> = {}): string {
  let line = `pwsh -NoProfile -File ${SCRIPTS}\\${script}`;
  // Quote values with spaces or commas, and values starting with "-" (pwsh -File would read
  // "--pgr4_native_profile" as a parameter name, not as the value of -Extra).
  for (const [k, v] of Object.entries(params)) if (v !== undefined && v !== "") line += ` -${k} ${/[\s,]|^-/.test(v) ? `"${v}"` : v}`;
  for (const [k, on] of Object.entries(switches)) if (on) line += ` -${k}`;
  return line;
}

async function launch(commandLine: string, wait: boolean, timeout?: string): Promise<void> {
  const job = await spawn(commandLine);
  if (wait) await waitJob(job, timeout);
  else console.log(`then: lab pgr4 wait ${job}   (as a background command)`);
}

/** Copies files from the host into .cache/pgr4/<kind>/<name>/. */
async function fetchFiles(files: string[], kind: string, name: string): Promise<void> {
  const local = join(await checkout(), ".cache", "pgr4", kind, name);
  mkdirSync(local, { recursive: true });
  for (const f of files) await sh(["fleet", "cp", `${HOST}:${f}`, join(local, f.split("\\").pop()!)], { quiet: true });
  console.log(`\ncopied ${files.length} files to ${local}`);
}

const fileLines = (out: string): string[] =>
  out.split(/\r?\n/).filter((l) => l.startsWith("FILE ")).map((l) => l.slice(5).trim());

/** Runs oracle_diff.py on the host for an oracle run's captures and copies the results here. */
async function diff(what: string, tiles: string, gain: string): Promise<void> {
  const dir = /[\\:]/.test(what) ? what : `C:\\recomp\\logs\\pgr4-${what}-captures`;
  const name = /[\\:]/.test(what) ? what.split("\\").filter(Boolean).pop()! : what;
  const out = await runScript("pgr4-diff", { dir, tiles, gain });
  process.stdout.write(out.split(/\r?\n/).filter((l) => !l.startsWith("FILE ")).join("\n"));
  await fetchFiles(fileLines(out), "diff", name);
}

/** Copies a demo's main clip (sbs.mp4, else native.mp4, else xenos.mp4) and its stills here. */
async function demoGet(tag: string): Promise<void> {
  const files = fileLines(await runScript("pgr4-ls", { dir: `C:\\recomp\\logs\\pgr4-demo-${tag}`, include: ["*.mp4", "still-*.png"] }));
  const clip = ["sbs.mp4", "native.mp4", "xenos.mp4"].map((n) => files.find((f) => f.endsWith(`\\${n}`))).find(Boolean);
  await fetchFiles([...(clip ? [clip] : []), ...files.filter((f) => f.endsWith(".png"))], "demo", tag);
}

async function installScripts(): Promise<void> {
  const top = await checkout();
  const pick = (dir: string, exts: string[]): string[] =>
    readdirSync(join(top, PGR4, dir)).filter((f) => exts.some((e) => f.endsWith(e))).map((f) => join(top, PGR4, dir, f));
  const sets: [string[], string][] = [
    [[...pick("lab", [".ps1"]), ...pick(join("native", "tools"), [".py"])], SCRIPTS],
    [pick("run", [".ps1", ".keys"]), RUN_SCRIPTS],
  ];
  for (const [files, dest] of sets) {
    await sh(["fleet", "exec", HOST, `New-Item -ItemType Directory -Force '${dest}' | Out-Null`], { quiet: true });
    for (const f of files) await sh(["fleet", "cp", f, `${HOST}:${dest}\\${f.split("/").pop()}`], { quiet: true });
    console.log(`installed ${files.length} files into ${dest}`);
    process.stdout.write(await runScript("pgr4-parsecheck", { dir: dest }));
  }
}

export async function pgr4(args: string[]): Promise<void> {
  const [verb, ...rest] = args;
  const pos = positional(rest);
  const wait = rest.includes("--wait");
  const owner = flag(rest, "--owner") ?? process.env.RECOMP_AGENT ?? "agent";
  const need = (n: number, usage: string): string[] => {
    if (pos.length < n) throw new Error(`usage: lab pgr4 ${usage}`);
    return pos;
  };
  switch (verb) {
    case "scripts":
      await installScripts();
      break;
    case "sync": {
      const [tree] = need(1, "sync <tree> [--ref R]");
      await sync(tree!, flag(rest, "--ref"));
      break;
    }
    case "build": {
      const [tree] = need(1, "build <tree> [--sdk] [--ref R] [--no-gate] [--wait]");
      await sync(tree!, flag(rest, "--ref"));
      const stage = rest.includes("--sdk") ? "prep,sdk,game,install" : undefined;
      await launch(ps("perf_build.ps1", { Dest: treePath(tree!), Stage: stage }, { NoGate: rest.includes("--no-gate") }), wait);
      break;
    }
    case "profile": {
      const [tree, tag] = need(2, "profile <tree> <tag> [--extra F] [--no-build] [--ref R] [--wait]");
      const noBuild = rest.includes("--no-build");
      if (!noBuild) await sync(tree!, flag(rest, "--ref"));
      await launch(ps("perf_buildrun.ps1", { Dest: treePath(tree!), Tag: tag, Owner: owner, Extra: flag(rest, "--extra") }, { NoBuild: noBuild }), wait);
      break;
    }
    case "compare": {
      const [tree] = need(1, "compare <tree> [--modes primary,none] [--extra F] [--suffix S] [--wait]");
      await launch(ps("perf_compare.ps1", { Dest: treePath(tree!), Owner: owner, Modes: flag(rest, "--modes"), Extra: flag(rest, "--extra"), Suffix: flag(rest, "--suffix") }), wait);
      break;
    }
    case "oracle": {
      const [tree, tag] = need(2, "oracle <tree> <tag> [--wait]");
      await launch(ps("oracle_q.ps1", { Dest: treePath(tree!), Tag: tag, Owner: owner }), wait);
      break;
    }
    case "check": {
      const [tree, prefix] = need(2, "check <tree> <prefix> [--no-build] [--ref R] [--extra F] [--wait]");
      const noBuild = rest.includes("--no-build");
      if (!noBuild) await sync(tree!, flag(rest, "--ref"));
      await launch(ps("int_chain.ps1", { Dest: treePath(tree!), Prefix: prefix, Extra: flag(rest, "--extra") }, { NoBuild: noBuild }), wait);
      break;
    }
    case "wait": {
      const [job] = need(1, "wait <job> [--timeout S]");
      await waitJob(job!, flag(rest, "--timeout"));
      break;
    }
    case "out": {
      const [tree, tag] = need(2, "out <tree> <tag> [--all]");
      process.stdout.write(await runScript("pgr4-out", { dest: treePath(tree!), tag, all: rest.includes("--all") }));
      break;
    }
    case "status":
      process.stdout.write(await runScript("pgr4-status"));
      break;
    case "trees":
      process.stdout.write(await runScript("pgr4-trees"));
      break;
    case "clean": {
      const [tree] = need(1, "clean <tree> [--force]");
      process.stdout.write(await runScript("pgr4-clean", { dest: treePath(tree!), force: rest.includes("--force") }));
      break;
    }
    case "ab": {
      const [treeA, treeB] = need(1, "ab <treeA> [<treeB>] [--a-ref R] [--b-ref R] [--a-extra F] [--b-extra F] [--rounds N] [--wait]");
      const aRef = flag(rest, "--a-ref"), bRef = flag(rest, "--b-ref");
      if (bRef && (!treeB || treeB === treeA)) throw new Error("--b-ref needs a second tree (one exe per tree)");
      if (aRef) await sync(treeA!, aRef);
      if (bRef) await sync(treeB!, bRef);
      await launch(ps("ab_run.ps1", {
        DestA: treePath(treeA!), DestB: treeB ? treePath(treeB) : undefined,
        ExtraA: flag(rest, "--a-extra"), ExtraB: flag(rest, "--b-extra"),
        ModeA: flag(rest, "--a-mode"), ModeB: flag(rest, "--b-mode"),
        Rounds: flag(rest, "--rounds"), Tag: flag(rest, "--tag"), Owner: owner,
      }, { BuildA: Boolean(aRef), BuildB: Boolean(bRef) }), wait, undefined);
      break;
    }
    case "diff": {
      const [what] = need(1, "diff <tag|dir> [--tiles 16x9] [--gain 8]");
      await diff(what!, flag(rest, "--tiles") ?? "16x9", flag(rest, "--gain") ?? "8");
      break;
    }
    case "demo": {
      const [tree] = need(1, "demo <tree> [--xenos] [--seconds 20] [--delay 8] [--uncapped] [--extra F] [--tag demo] [--wait]");
      const tag = flag(rest, "--tag") ?? "demo";
      await launch(ps("demo_run.ps1", {
        Dest: treePath(tree!), Tag: tag, Owner: owner, Extra: flag(rest, "--extra"),
        Seconds: flag(rest, "--seconds"), Delay: flag(rest, "--delay"),
      }, { Xenos: rest.includes("--xenos"), Uncapped: rest.includes("--uncapped") }), wait);
      if (wait) await demoGet(tag);
      break;
    }
    case "demo-get": {
      const [tag] = need(1, "demo-get <tag>");
      await demoGet(tag!);
      break;
    }
    case undefined:
    case "help":
    case "--help":
      console.log(PGR4_HELP);
      break;
    default:
      throw new Error(`unknown pgr4 verb "${verb}"\n\n${PGR4_HELP}`);
  }
}
