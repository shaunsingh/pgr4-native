/** Runs the host-side PowerShell helpers on the Windows build host through fleet. */
import { join } from "node:path";
import { HOST_SCRIPTS } from "./layout.ts";

export const HOST = process.env.RECOMP_HOST ?? "main";

/** Arguments travel as one base64 JSON token: dump names hold commas and
 *  parentheses that PowerShell would otherwise split or evaluate. */
export function encode(value: unknown): string {
  return Buffer.from(JSON.stringify(value), "utf8").toString("base64");
}

export async function runScript(name: string, arg?: unknown, opts: { timeout?: number } = {}): Promise<string> {
  const script = join(HOST_SCRIPTS, `${name}.ps1`);
  const args = ["exec", "--script", script, "--raw"];
  if (opts.timeout !== undefined) args.push("--timeout", String(opts.timeout));
  args.push(HOST);
  if (arg !== undefined) args.push("--", encode(arg));
  // FLEET_WIN_SESSION=0: the persistent Windows session sometimes loses a command's output.
  const proc = Bun.spawn(["fleet", ...args], { stdout: "pipe", stderr: "inherit",
    env: { ...process.env, FLEET_WIN_SESSION: process.env.FLEET_WIN_SESSION ?? "0" } });
  const out = await new Response(proc.stdout).text();
  const code = await proc.exited;
  if (code !== 0) throw new Error(`${name}.ps1 failed on ${HOST} (exit ${code})`);
  return out;
}
