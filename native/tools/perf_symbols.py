#!/usr/bin/env python3
"""Flat CPU profile of a game run under Proton, from `perf record` (deck-run.sh --perf).

perf cannot read the symbols of PE images, so samples in the recompiled exe and ReXGlue's DLLs
are resolved here with the lld-link map files that native/build.sh links them with (/MAP), at
the load addresses the game's /proc/<pid>/maps showed during the recording. Samples in other PE
images (Wine's ucrtbase, ntdll, d3d12core...) go to the nearest export at or below them, shown
as "name+". Samples elsewhere (Wine's Unix side, Mesa, libc) keep perf's own symbols.

  perf_symbols.py <perf.data> <maps> <module.map>... [--top 25] [--threads 6] [--undname CMD]

Prints the share of samples per module, the hottest functions over all threads, then the
hottest functions of the busiest threads (named by SetThreadDescription, truncated to 15
characters by Linux). --undname demangles MSVC names (default: llvm-undname when on PATH).
"""
import argparse
import bisect
import collections
import os
import re
import shutil
import struct
import subprocess
import sys

# lld-link (link.exe format): " 0001:00000000       ?Name@@...   0000000140001000 f   obj.obj"
MAP_SYMBOL = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\b")
MAP_BASE = re.compile(r"Preferred load address is ([0-9a-fA-F]+)")


class Module:
    def __init__(self, map_path: str):
        self.name = os.path.splitext(os.path.basename(map_path))[0]
        self.preferred = 0
        symbols = {}
        with open(map_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if m := MAP_BASE.search(line):
                    self.preferred = int(m.group(1), 16)
                elif m := MAP_SYMBOL.match(line):
                    va = int(m.group(2), 16)
                    if va >= self.preferred and self.preferred:
                        symbols.setdefault(va - self.preferred, m.group(1))
        self.rvas = sorted(symbols)
        self.names = [symbols[r] for r in self.rvas]
        self.base = None
        self.end = None
        self.size = image_size(map_path)

    def lookup(self, rva: int) -> str:
        i = bisect.bisect_right(self.rvas, rva) - 1
        return self.names[i] if i >= 0 else f"{self.name}+{rva:#x}"

    def start(self, rva: int) -> int:
        i = bisect.bisect_right(self.rvas, rva) - 1
        return self.rvas[i] if i >= 0 else 0


def image_size(map_path: str) -> int:
    """SizeOfImage of the PE next to the map file (same name, .exe or .dll); 0 if not found."""
    stem = os.path.splitext(map_path)[0]
    for ext in (".exe", ".dll"):
        try:
            with open(stem + ext, "rb") as f:
                head = f.read(4096)
        except OSError:
            continue
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        if head[pe:pe + 4] != b"PE\0\0":
            return 0
        return struct.unpack_from("<I", head, pe + 24 + 56)[0]  # optional header + 56
    return 0


class PeExports:
    """Export table of a PE image on disk: a module without a map file or symbols."""

    def __init__(self, path: str, base: int):
        self.base = base
        self.rvas, self.names = [], []
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return
        opt = pe + 24
        sections_at = opt + struct.unpack_from("<H", data, pe + 20)[0]
        sections = [struct.unpack_from("<IIII", data, sections_at + 40 * i + 8)
                    for i in range(struct.unpack_from("<H", data, pe + 6)[0])]

        def offset(rva: int) -> int:
            for vsize, va, rawsize, raw in sections:
                if va <= rva < va + max(vsize, rawsize):
                    return rva - va + raw
            return rva

        data_dirs = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
        export_rva, export_size = struct.unpack_from("<II", data, data_dirs)
        if not export_rva:
            return
        e = offset(export_rva)
        count, functions, names, ordinals = struct.unpack_from("<4I", data, e + 24)
        function_rvas = struct.unpack_from(f"<{struct.unpack_from('<I', data, e + 20)[0]}I", data,
                                           offset(functions))
        symbols = {}
        for i in range(count):
            name_at = offset(struct.unpack_from("<I", data, offset(names) + 4 * i)[0])
            name = data[name_at:data.index(b"\0", name_at)].decode("ascii", "replace")
            rva = function_rvas[struct.unpack_from("<H", data, offset(ordinals) + 2 * i)[0]]
            if not export_rva <= rva < export_rva + export_size:  # not a forwarder
                symbols.setdefault(rva, name)
        self.rvas = sorted(symbols)
        self.names = [symbols[r] + "+" for r in self.rvas]

    def lookup(self, ip: int):
        i = bisect.bisect_right(self.rvas, ip - self.base) - 1
        return self.names[i] if i >= 0 else None


def pe_bases(maps_path: str) -> dict:
    """Lowest mapping (the image header) of every .dll/.exe file in the maps."""
    bases = {}
    for line in open(maps_path):
        parts = line.split(maxsplit=5)
        if len(parts) == 6 and parts[5].strip().lower().endswith((".dll", ".exe")):
            path = parts[5].strip()
            start = int(parts[0].split("-")[0], 16)
            bases[path] = min(bases.get(path, start), start)
    return bases


def load_bases(maps_path: str, modules) -> None:
    """Each module's image: from its lowest file mapping (the header) over SizeOfImage. Wine
    copies sections that are not page-aligned in the file into anonymous memory, so the file
    mappings alone can miss the code."""
    for line in open(maps_path):
        parts = line.split(maxsplit=5)
        if len(parts) < 6:
            continue
        path = parts[5].strip().replace("\\", "/")
        start, end = (int(x, 16) for x in parts[0].split("-"))
        for mod in modules:
            if os.path.basename(path).lower().startswith(mod.name.lower() + "."):
                if mod.base is None or start < mod.base:
                    mod.base = start
                mod.end = max(mod.end or 0, end)
    for mod in modules:
        if mod.base is not None and mod.size:
            mod.end = max(mod.end, mod.base + mod.size)


def undname(names, command):
    if not command or not names:
        return {n: n for n in names}
    mangled = [n for n in names if n.startswith("?")]
    out = {n: n for n in names}
    if mangled:
        r = subprocess.run(command.split(), input="\n".join(mangled) + "\n", capture_output=True,
                           text=True)
        # llvm-undname echoes each input line, then its demangled form and a blank line.
        lines = [l for l in r.stdout.splitlines() if l.strip()]
        it = iter(lines)
        for raw, echoed in zip(it, it):
            if raw in out:
                out[raw] = echoed
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("maps")
    ap.add_argument("module_maps", nargs="+")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--undname", default=shutil.which("llvm-undname") or "")
    ap.add_argument("--annotate", metavar="TEXT",
                    help="also list the busiest instructions (RVA, offset in the function) of the "
                         "mapped functions whose name contains TEXT, for llvm-objdump")
    args = ap.parse_args()

    modules = [Module(p) for p in args.module_maps]
    load_bases(args.maps, modules)
    for m in modules:
        if m.base is None:
            print(f"{m.name}: not in {args.maps}; its samples stay unresolved", file=sys.stderr)
    modules = [m for m in modules if m.base is not None]
    bases = pe_bases(args.maps)
    exports = {}

    script = subprocess.run(["perf", "script", "-i", args.data, "-F", "comm,tid,ip,sym,dso"],
                            capture_output=True, text=True, errors="replace")
    if script.returncode:
        sys.exit(script.stderr.strip())
    # "  comm  tid  ip sym (dso)"; comm may contain spaces, so split on the tid.
    sample = re.compile(r"^\s*(.*?)\s+(\d+)\s+([0-9a-f]+)\s+(.*?)\s*\((.*)\)\s*$")
    by_thread = collections.defaultdict(collections.Counter)
    by_symbol = collections.Counter()
    by_module = collections.Counter()
    by_ip = collections.Counter()
    names = {}
    total = 0
    for line in script.stdout.splitlines():
        m = sample.match(line)
        if not m:
            continue
        comm, tid, ip, sym, dso = m.group(1), m.group(2), int(m.group(3), 16), m.group(4), m.group(5)
        mod = next((x for x in modules if x.base <= ip < x.end), None)
        if mod:
            key = (mod.name, mod.lookup(ip - mod.base))
            if args.annotate and args.annotate in key[1]:
                rva = ip - mod.base
                by_ip[(mod.name, key[1], mod.start(rva), rva)] += 1
        elif ip >= 0xFFFF800000000000:
            key = ("[kernel]", "[kernel]")
        else:
            dso_name = os.path.basename(dso) if dso else "[unknown]"
            if (not sym or sym == "[unknown]") and dso in bases:
                if dso not in exports:
                    exports[dso] = PeExports(dso, bases[dso])
                sym = exports[dso].lookup(ip)
            key = (dso_name, sym if sym and sym != "[unknown]" else dso_name)
        total += 1
        by_symbol[key] += 1
        by_module[key[0]] += 1
        thread = f"{comm} ({tid})"
        by_thread[thread][key] += 1
        names[key[1]] = key[1]
    if not total:
        sys.exit(f"{args.data}: no samples")
    names = undname(list(names), args.undname)

    def show(counter, n, of):
        for (mod, sym), c in counter.most_common(n):
            print(f"  {100.0 * c / of:5.1f} %  {names.get(sym, sym)[:110]}  [{mod}]")

    print(f"{args.data}: {total} samples")
    print("modules:")
    for mod, c in by_module.most_common(12):
        print(f"  {100.0 * c / total:5.1f} %  {mod}")
    print(f"functions (all threads, % of all samples):")
    show(by_symbol, args.top, total)
    threads = sorted(by_thread.items(), key=lambda kv: -sum(kv[1].values()))
    for thread, counter in threads[:args.threads]:
        n = sum(counter.values())
        print(f"{thread}: {100.0 * n / total:.1f} % of samples (% of the thread's below)")
        show(counter, args.top, n)
    if by_ip:
        functions = collections.Counter()
        for (mod, sym, start, _), c in by_ip.items():
            functions[(mod, sym, start)] += c
        for (mod, sym, start), n in functions.most_common(8):
            print(f"{names.get(sym, sym)[:100]} [{mod}] at RVA {start:#x}: {n} samples")
            ips = [(rva, c) for (m, s, _, rva), c in by_ip.items() if (m, s) == (mod, sym)]
            for rva, c in sorted(ips, key=lambda x: -x[1])[:args.top]:
                print(f"  {100.0 * c / n:5.1f} %  RVA {rva:#x}  +{rva - start:#x}")


if __name__ == "__main__":
    main()
