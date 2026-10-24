#!/usr/bin/env python3
"""Packages a release ZIP of the native-renderer build from an explicit file list.

    native/tools/package_release.py [--dest DIR] [--version vX.Y.Z] [--out DIR]

--dest is the build tree native/build.sh wrote (default ~/recomp/builds/pgr4/native-deck): the
program comes from its run/runtime, the license texts from its MTR-PGR4 checkout. The ZIP,
pgr4-native-<version>-windows-x64.zip in --out (default out/ in the repository), holds one
folder of that name with the program, its settings (native/release/pgr4_recompiled.toml), the
player README (native/release/README.md) and the licenses. The player adds game/ and
title-update/ (README.md).

The package refuses game data and anything else outside the list, and an instrumented (PGO
--pgo gen) build. Entries are sorted and dated at the commit's time, so a build reproduces its
ZIP byte for byte.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PROGRAM = ("pgr4_recompiled.exe", "rexruntime.dll", "rexgpu-xenos.dll")
ALLOWED_SUFFIXES = {".exe", ".dll", ".toml", ".txt", ".md"}


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def pe_sections(path: Path) -> list[str]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit(f"{path}: not a PE image")
    count, optional = struct.unpack_from("<H12xH", data, pe + 6)
    table = pe + 24 + optional
    return [data[table + 40 * i:table + 40 * i + 8].rstrip(b"\0").decode("ascii", "replace")
            for i in range(count)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dest", type=Path,
                        default=Path.home() / "recomp/builds/pgr4/native-deck")
    parser.add_argument("--version", default=None, help="default: git describe --tags --dirty")
    parser.add_argument("--out", type=Path, default=REPO / "out")
    args = parser.parse_args()

    version = args.version or git("describe", "--tags", "--always", "--dirty")
    name = f"pgr4-native-{version}-windows-x64"
    runtime = args.dest / "run/runtime"
    mtr = args.dest / "MTR-PGR4"

    # Package path -> source path, the whole list.
    files: dict[str, Path] = {f: runtime / f for f in PROGRAM}
    files["pgr4_recompiled.toml"] = REPO / "native/release/pgr4_recompiled.toml"
    files["README.md"] = REPO / "native/release/README.md"
    files["LICENSE.txt"] = REPO / "LICENSE"
    files["LICENSES/pgr4-native-THIRD_PARTY_NOTICES.txt"] = REPO / "THIRD_PARTY_NOTICES.md"
    files["LICENSES/MTR-PGR4-MIT.txt"] = mtr / "LICENSE"
    files["LICENSES/MTR-PGR4-THIRD_PARTY.txt"] = mtr / "THIRD_PARTY.md"
    for license_file in sorted((mtr / "LICENSES").glob("*.txt")):
        files[f"LICENSES/{license_file.name}"] = license_file

    if missing := [str(p) for p in files.values() if not p.is_file()]:
        sys.exit("missing: " + ", ".join(missing))
    if bad := [f for f in files if Path(f).suffix.lower() not in ALLOWED_SUFFIXES
               or (f.endswith(".md") and f != "README.md")]:
        sys.exit("excluded data in the package: " + ", ".join(bad))
    if any(s.startswith(".lprf") for s in pe_sections(files["pgr4_recompiled.exe"])):
        sys.exit("pgr4_recompiled.exe is an instrumented (--pgo gen) build")

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH") or git("log", "-1", "--format=%ct"))
    stamp = time.gmtime(max(epoch, 315532800))[:6]  # ZIP dates start in 1980
    args.out.mkdir(parents=True, exist_ok=True)
    zip_path = args.out / f"{name}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for member in sorted(files):
            info = zipfile.ZipInfo(f"{name}/{member}", stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, files[member].read_bytes())
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (args.out / f"{name}.zip.sha256").write_text(f"{digest}  {zip_path.name}\n")
    print(f"{zip_path} ({len(files)} files, {zip_path.stat().st_size >> 20} MiB)\n{digest}")


if __name__ == "__main__":
    main()
