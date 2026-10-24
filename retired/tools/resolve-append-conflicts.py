#!/usr/bin/env python3
"""Resolves merge conflicts where both sides only appended text (an empty diff3 base), by keeping
ours, a blank line, then theirs. Any other conflict block is left in place, and the script exits 1
if one remains.

Agents append their own `#### <Name>` section at the end of STATE.md (and the worklog, which has
`merge=union` in .gitattributes), so merging two agent branches stops on a tail conflict whose base
is empty. Needs diff3-style markers (`git config merge.conflictStyle diff3` or zdiff3).

    git merge origin/<branch>
    tools/resolve-append-conflicts.py path/to/STATE.md
    git add path/to/STATE.md && git commit --no-edit
"""
import re
import sys

BLOCK = re.compile(
    r"^<<<<<<< [^\n]*\n(?P<ours>.*?)^\|\|\|\|\|\|\| [^\n]*\n(?P<base>.*?)^=======\n(?P<theirs>.*?)^>>>>>>> [^\n]*\n",
    re.S | re.M,
)


def resolve(text: str) -> tuple[str, int, int]:
    done = left = 0

    def repl(m: re.Match) -> str:
        nonlocal done, left
        if m.group("base").strip():
            left += 1
            return m.group(0)
        done += 1
        ours, theirs = m.group("ours").rstrip("\n"), m.group("theirs").strip("\n")
        return f"{ours}\n\n{theirs}\n" if ours else f"{theirs}\n"

    return BLOCK.sub(repl, text), done, left


def main() -> int:
    remaining = 0
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        new, done, left = resolve(text)
        if done:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
        if "<<<<<<< " in new and not left:
            print(f"{path}: conflict markers without a diff3 base; set merge.conflictStyle diff3", file=sys.stderr)
            left += 1
        print(f"{path}: {done} append conflict(s) resolved, {left} left")
        remaining += left
    return 1 if remaining else 0


if __name__ == "__main__":
    sys.exit(main())
