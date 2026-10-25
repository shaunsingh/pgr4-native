#!/usr/bin/env python3
"""Packages a release ZIP of the native-renderer build from an explicit file list.

    native/tools/package_release.py [--dest DIR] [--version vX.Y.Z] [--out DIR]

--dest is the build tree native/build.sh wrote (default ~/recomp/builds/pgr4/native-deck): the
programs come from its run/runtime, the libraries' license texts from its ReXGlue checkout
(rexglue-sdk/). The ZIP, pgr4-native-<version>-windows-x64.zip in --out (default out/ in the
repository), holds one folder of that name with the programs, their settings
(native/release/*.toml), the player README (native/release/README.md), the licenses, and
LICENSES/SOURCE.txt: the commits the program was built from, which the LGPL of FFmpeg and
libmspack asks for. The player adds game/, with the title update in game/Content/ (README.md).

The package refuses game data and anything else outside the list, an instrumented (PGO --pgo
gen) build, and a runtime DLL linked with a library LICENSES below does not cover. Entries are
sorted and dated at the commit's time, so a build reproduces its ZIP byte for byte.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PROGRAM = ("pgr4_recompiled.exe", "gw_recompiled.exe", "rexruntime.dll", "rexgpu-xenos.dll")
SETTINGS = ("pgr4_recompiled.toml", "gw_recompiled.toml")
ALLOWED_SUFFIXES = {".exe", ".dll", ".toml", ".txt", ".md"}

# The licenses of the code built into the programs that ship as files in the SDK tree:
# LICENSES/<name>.txt <- rexglue-sdk/<path>. Code whose license is in its source files only
# (disruptorplus, RenderDoc's header, stb_image, the DXC headers, the DXBC checksum) is in
# THIRD_PARTY_NOTICES.md.
LICENSES = {
    "ReXGlue-SDK": "LICENSE",
    "CLI11": "thirdparty/cli11/LICENSE",
    "FFmpeg-LGPL-2.1": "thirdparty/FFmpeg/COPYING.LGPLv2.1",
    "FFmpeg": "thirdparty/FFmpeg/LICENSE.md",
    "fmt": "thirdparty/fmt/LICENSE",
    "hidapi-BSD": "thirdparty/sdl3/src/hidapi/LICENSE-bsd.txt",
    "imgui": "thirdparty/imgui/LICENSE.txt",
    "libmspack-LGPL-2.1": "thirdparty/libmspack/libmspack/COPYING.LIB",
    "aes_128": "thirdparty/aes_128/LICENSE",
    "o1heap": "thirdparty/o1heap/LICENSE",
    "SDL3": "thirdparty/sdl3/LICENSE.txt",
    "simde": "thirdparty/simde/COPYING",
    "spdlog": "thirdparty/spdlog/LICENSE",
    "tomlplusplus": "thirdparty/tomlplusplus/LICENSE",
    "utfcpp": "thirdparty/utfcpp/LICENSE",
    "xxHash": "thirdparty/xxHash/LICENSE",
}
# The SDK's static libraries the runtime DLLs link, by the LICENSES entries that cover them.
LINKED = {
    "SDL3-static": ("SDL3", "hidapi-BSD"),
    "aes128": ("aes_128",),
    "fmt": ("fmt",),
    "libavcodec": ("FFmpeg", "FFmpeg-LGPL-2.1"),
    "libavformat": ("FFmpeg", "FFmpeg-LGPL-2.1"),
    "libavutil": ("FFmpeg", "FFmpeg-LGPL-2.1"),
    "mspack": ("libmspack-LGPL-2.1",),
    "o1heap": ("o1heap",),
    "rexruntime": ("ReXGlue-SDK",),
    "spdlog": ("spdlog",),
    "xxhash": ("xxHash",),
}
WINDOWS_LIBS = {
    "advapi32", "bcrypt", "comdlg32", "d3d12", "dbghelp", "dinput8", "dxgi", "dxguid", "gdi32",
    "imm32", "kernel32", "ole32", "oleaut32", "setupapi", "shell32", "user32", "uuid", "version",
    "winmm", "winspool", "ws2_32",
}


def git(tree: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(tree), *args], check=True, capture_output=True,
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


def check_linked(build_ninja: Path) -> None:
    """Every library the runtime DLLs link is a Windows one or covered by LICENSES."""
    text = build_ninja.read_text()
    for dll in ("rexruntime.dll", "rexgpu-xenos.dll"):
        block = re.search(rf"^build [^\n:|]*[/\\]{re.escape(dll)}[ :|][^\n]*\n((?:[ \t]+[^\n]*\n)*)",
                          text, re.M)
        libs = re.search(r"^\s+LINK_LIBRARIES = (.*)$", block.group(1), re.M) if block else None
        if not libs:
            sys.exit(f"{build_ninja}: no link line for {dll}")
        names = {Path(lib).stem for lib in re.findall(r"[^\s\"]+\.lib", libs.group(1))}
        if unknown := sorted(n for n in names if n.lower() not in WINDOWS_LIBS and n not in LINKED):
            sys.exit(f"{dll} links {', '.join(unknown)}: add their licenses to package_release.py")


def source_note(sdk: Path) -> str:
    series = (sdk / ".pgr4-series").read_text().strip()
    lines = [
        "The program in this package was built from these sources.",
        "",
        f"pgr4-native  https://github.com/shaunsingh/pgr4-native  {git(REPO, 'rev-parse', 'HEAD')}",
        f"ReXGlue SDK  {git(sdk, 'remote', 'get-url', 'origin')}  {git(sdk, 'rev-parse', 'HEAD')}",
        f"             with pgr4-native's sdk/patches/ (series {series}) applied",
    ]
    for sub in ("thirdparty/FFmpeg", "thirdparty/libmspack"):
        url = git(sdk, "config", "-f", ".gitmodules", f"submodule.{sub}.url")
        lines.append(f"{sub.split('/')[1]:<12} {url}  {git(sdk / sub, 'rev-parse', 'HEAD')}")
    lines += [
        "",
        "FFmpeg (libavutil, libavcodec, libavformat) and libmspack are statically linked into",
        "rexruntime.dll under the GNU Lesser General Public License 2.1 (LICENSES/FFmpeg-LGPL-2.1.txt,",
        "LICENSES/libmspack-LGPL-2.1.txt). To use a modified version of either, rebuild rexruntime.dll",
        "from the sources above (pgr4-native's native/build.sh --stage sdk) and replace the one here.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dest", type=Path,
                        default=Path.home() / "recomp/builds/pgr4/native-deck")
    parser.add_argument("--version", default=None, help="default: git describe --tags --dirty")
    parser.add_argument("--out", type=Path, default=REPO / "out")
    args = parser.parse_args()

    version = args.version or git(REPO, "describe", "--tags", "--always", "--dirty")
    name = f"pgr4-native-{version}-windows-x64"
    runtime = args.dest / "run/runtime"
    sdk = args.dest / "rexglue-sdk"

    # Package path -> source path or contents, the whole list.
    files: dict[str, Path | bytes] = {f: runtime / f for f in PROGRAM}
    files.update({f: REPO / "native/release" / f for f in SETTINGS})
    files["README.md"] = REPO / "native/release/README.md"
    files["LICENSE.txt"] = REPO / "LICENSE"
    files["LICENSES/THIRD_PARTY_NOTICES.txt"] = REPO / "THIRD_PARTY_NOTICES.md"
    files.update({f"LICENSES/{n}.txt": sdk / p for n, p in LICENSES.items()})

    if missing := [str(p) for p in files.values() if isinstance(p, Path) and not p.is_file()]:
        sys.exit("missing: " + ", ".join(missing))
    if bad := [f for f in files if Path(f).suffix.lower() not in ALLOWED_SUFFIXES
               or (f.endswith(".md") and f != "README.md")]:
        sys.exit("excluded data in the package: " + ", ".join(bad))
    for exe in ("pgr4_recompiled.exe", "gw_recompiled.exe"):
        if any(s.startswith(".lprf") for s in pe_sections(files[exe])):
            sys.exit(f"{exe} is an instrumented (--pgo gen) build")
    check_linked(sdk / "out/build/native-release/build.ninja")
    files["LICENSES/SOURCE.txt"] = source_note(sdk).encode()

    epoch = int(os.environ.get("SOURCE_DATE_EPOCH") or git(REPO, "log", "-1", "--format=%ct"))
    stamp = time.gmtime(max(epoch, 315532800))[:6]  # ZIP dates start in 1980
    args.out.mkdir(parents=True, exist_ok=True)
    zip_path = args.out / f"{name}.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for member in sorted(files):
            info = zipfile.ZipInfo(f"{name}/{member}", stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            src = files[member]
            z.writestr(info, src if isinstance(src, bytes) else src.read_bytes())
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (args.out / f"{name}.zip.sha256").write_text(f"{digest}  {zip_path.name}\n")
    print(f"{zip_path} ({len(files)} files, {zip_path.stat().st_size >> 20} MiB)\n{digest}")


if __name__ == "__main__":
    main()
