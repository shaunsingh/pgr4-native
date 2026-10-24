#!/usr/bin/env bun
/** lab: build, run, profile and check the PGR4 native renderer on a Windows build host.
 *  `lab <verb> ...` is `lab pgr4 <verb> ...` in the workspace CLI; `lab pgr4 <verb>` works too. */
import { pgr4 } from "./pgr4.ts";

const args = process.argv.slice(2);
try {
  await pgr4(args[0] === "pgr4" ? args.slice(1) : args);
} catch (err) {
  console.error(`lab: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
}
