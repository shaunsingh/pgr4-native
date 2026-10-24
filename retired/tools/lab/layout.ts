/** Where the lab finds things. The same files run in two layouts: the private workspace
 *  (src/*.ts, {native,instrument,lab,run}, main/pgr4-*.ps1) and the public
 *  pgr4-native repository (tools/lab/*.ts, {native,instrument,lab,run}/ at the root,
 *  tools/lab/host/pgr4-*.ps1). */
import { existsSync } from "node:fs";
import { join } from "node:path";

const priv = existsSync(join(import.meta.dir, "..", "projects"));
/** Repository root of this checkout of the lab. */
export const ROOT = priv ? join(import.meta.dir, "..") : join(import.meta.dir, "..", "..");
/** The PGR4 project folder (native/, instrument/, lab/, run/) relative to a git top. */
export const PGR4 = priv ? join("projects", "pgr4") : ".";
/** Host-side PowerShell helpers run through `runScript`. */
export const HOST_SCRIPTS = priv ? join(ROOT, "main") : join(import.meta.dir, "host");
