#!/usr/bin/env python3
"""Adds the case variants of Windows SDK headers and libraries that a source tree uses to an
xwin splat, as symlinks beside the real files.

Windows file systems ignore case, so Windows sources spell SDK names freely (ObjBase.h for
objbase.h, Dbghelp.lib for dbghelp.lib). xwin links the variants the SDK's own headers use; this
covers the ones in the sources being built. Re-runnable: existing names are left alone.

usage: xwin-case-links.py <xwin splat> <source dir>...
"""
import os
import re
import sys

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
PRAGMA_LIB = re.compile(rb'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"', re.I)
CMAKE_LIB = re.compile(rb"\b([A-Za-z][A-Za-z0-9_]*)(?:\.lib)?\b")
SOURCE_EXT = {".h", ".hpp", ".hh", ".hxx", ".inl", ".inc", ".c", ".cc", ".cpp", ".cxx", ".ipp"}
INCLUDE_DIRS = ["crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared", "sdk/include/winrt"]
LIB_DIRS = ["crt/lib/x86_64", "sdk/lib/um/x86_64", "sdk/lib/ucrt/x86_64"]


def folded_index(root: str) -> dict:
    """Lower-cased relative path -> real relative path, for every file under root."""
    index = {}
    for dirpath, _, files in os.walk(root):
        for name in files:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            index.setdefault(rel.lower(), rel)
    return index


def scan(dirs):
    """Header names the sources include that they do not ship themselves, and library names."""
    headers, libs, own = set(), set(), set()
    for top in dirs:
        for dirpath, subdirs, files in os.walk(top):
            subdirs[:] = [d for d in subdirs if d not in (".git", "out", "build", "generated")]
            for name in files:
                own.add(name.lower())
                ext = os.path.splitext(name)[1].lower()
                path = os.path.join(dirpath, name)
                if ext in SOURCE_EXT:
                    data = open(path, "rb").read()
                    headers.update(m.decode(errors="replace").replace("\\", "/") for m in INCLUDE.findall(data))
                    libs.update(m.decode(errors="replace") for m in PRAGMA_LIB.findall(data))
                elif name == "CMakeLists.txt" or ext == ".cmake":
                    data = open(path, "rb").read()
                    for block in re.findall(rb"target_link_libraries\s*\(([^)]*)\)", data):
                        libs.update(m.decode() for m in CMAKE_LIB.findall(block))
    return {h for h in headers if os.path.basename(h).lower() not in own}, libs


def link(root: str, index: dict, wanted: str) -> bool:
    real = index.get(wanted.lower())
    if real is None or real == wanted or os.path.lexists(os.path.join(root, wanted)):
        return False
    target = os.path.join(root, wanted)
    if os.path.dirname(real).lower() != os.path.dirname(wanted).lower():
        return False
    # A variant in a differently cased directory needs that directory first; not seen so far.
    if not os.path.isdir(os.path.dirname(target)):
        return False
    os.symlink(os.path.basename(real), target)
    return True


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    splat, dirs = sys.argv[1], sys.argv[2:]
    headers, libs = scan(dirs)
    made = []
    for sub in INCLUDE_DIRS:
        root = os.path.join(splat, sub)
        index = folded_index(root)
        made += [f"{sub}/{h}" for h in sorted(headers) if link(root, index, h)]
    lib_names = {l if l.lower().endswith(".lib") else l + ".lib" for l in libs}
    for sub in LIB_DIRS:
        root = os.path.join(splat, sub)
        index = folded_index(root)
        made += [f"{sub}/{l}" for l in sorted(lib_names) if link(root, index, l)]
    for m in made:
        print(f"linked {m}")
    print(f"xwin-case-links: {len(made)} new, {len(headers)} includes and {len(lib_names)} libraries scanned")


if __name__ == "__main__":
    main()
