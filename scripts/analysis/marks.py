#!/usr/bin/env python3
"""What the screen showed in the seconds before each mark of a play session (F12).

A mark comes a few seconds after what it marks, so for each "PGR4 native mark at frame N" line
of the session log this looks at the frames shown in the --before seconds up to N (and --after
past it) in the glitch catcher's mark-<N>.rec. Held frames, which the screen never showed, are
left out. Per mark it prints the slow frames of the window with the frame CSV's causes, the
blocks that flash (flicker.py's events) and whole-image jumps, each with how long before the
mark it was, and writes into --out/mark-<N>/ an overview sheet of the window and a sheet per
event and jump.

  marks.py <session prefix> [--before 5] [--after 0.5] [--out DIR] [--top 4] [--mark N ...]

<session prefix>: the session's path without .log, e.g. ~/pgr4-proton/play/logs/20261022-141955
(the log, <prefix>-frames.csv and <prefix>-mark-<N>.rec). Needs numpy.
"""

import argparse
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flicker import (HEADER, block_means, find_events, find_jumps, load_csv,  # noqa: E402
                     open_rec, outline, write_png)

MARK = re.compile(r"PGR4 native mark at frame (\d+)")
CAUSES = ["held", "pso_created", "pso_wait_us", "swap_fence_us", "hook_wait_us", "draw_us",
          "tex_evicted", "textures_missing"]



def window(recs, mark, before, after, held):
    """Rows (header and pixels), frame indices and times of the frames shown around the mark."""
    rows, frames, times = [], [], []
    t_mark = None
    for mm, w, h, fr, tm in recs:
        hit = np.nonzero(fr == mark)[0]
        if hit.size:
            t_mark = int(tm[hit[0]])
    if t_mark is None:
        return None
    for mm, w, h, fr, tm in recs:
        sel = np.nonzero((tm >= t_mark - before * 1e6) & (tm <= t_mark + after * 1e6))[0]
        for i in sel:
            if int(fr[i]) in held:
                continue
            rows.append(np.asarray(mm[i]))
            frames.append(int(fr[i]))
            times.append((int(tm[i]) - t_mark) / 1e6)
    if not rows:
        return None
    order = np.argsort(frames)
    return (np.stack([rows[i] for i in order]), w, h, [frames[i] for i in order],
            [times[i] for i in order])


def tile(rows, i, w, h):
    return rows[i, HEADER.size:].reshape(h, w, 3)


def overview(rows, w, h, picks):
    tw, th = w // 2, h // 2
    sheet = np.zeros((th * 3, tw * 3, 3), dtype=np.uint8)
    for j, i in enumerate(picks[:9]):
        p = tile(rows, i, w, h)[: th * 2, : tw * 2]
        small = p.reshape(th, 2, tw, 2, 3).mean(axis=(1, 3)).astype(np.uint8)
        sheet[(j // 3) * th:(j // 3 + 1) * th, (j % 3) * tw:(j % 3 + 1) * tw] = small
    return sheet


def causes(row):
    return " ".join(f"{c} {row.get(c, '?')}" for c in CAUSES if row.get(c, "0") not in ("0", "0.0"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--before", type=float, default=5.0)
    ap.add_argument("--after", type=float, default=0.5)
    ap.add_argument("--out")
    ap.add_argument("--top", type=int, default=4)
    ap.add_argument("--mark", type=int, nargs="*")
    ap.add_argument("--block", type=int, default=10)
    ap.add_argument("--search", type=int, default=2)
    ap.add_argument("--threshold", type=float, default=40.0)
    ap.add_argument("--max-len", type=int, default=4)
    a = ap.parse_args()
    prefix = os.path.expanduser(a.prefix)
    with open(prefix + ".log", errors="replace") as f:
        marks = [int(m.group(1)) for m in map(MARK.search, f) if m]
    if a.mark:
        marks = [m for m in marks if m in a.mark] or a.mark
    csv_path = prefix + "-frames.csv"
    rows_csv = load_csv(csv_path) if os.path.exists(csv_path) else {}
    held = {f for f, r in rows_csv.items() if r.get("held") == "1"}
    out = os.path.expanduser(a.out or prefix + "-marks")
    print(f"{len(marks)} marks in {prefix}.log: {' '.join(map(str, marks))}")

    for mark in marks:
        dump = prefix + f"-mark-{mark}.rec"
        got = os.path.exists(dump) and window([open_rec(dump)], mark, a.before, a.after, held)
        if not got:
            print(f"\nmark {mark}: no {dump}")
            continue
        rows, w, h, frames, times = got
        d = os.path.join(out, f"mark-{mark}")
        os.makedirs(d, exist_ok=True)
        n = len(frames)
        print(f"\nmark {mark}: {n} frames shown, {times[0]:.1f} to {times[-1]:+.1f} s ({d})")

        ms = [float(rows_csv[f]["frame_ms"]) for f in frames if f in rows_csv]
        if ms:
            med = float(np.median(ms))
            slow = [f for f in frames if f in rows_csv and float(rows_csv[f]["frame_ms"]) > max(25.0, 2 * med)]
            held_in = [f for f in held if frames[0] <= f <= frames[-1]]
            print(f"  frame time median {med:.1f} ms, {len(slow)} frames over {max(25.0, 2 * med):.0f} ms, "
                  f"{len(held_in)} held")
            for f in sorted(slow, key=lambda f: -float(rows_csv[f]["frame_ms"]))[: a.top]:
                t = times[frames.index(f)]
                print(f"    {t:+5.2f} s frame {f}: {float(rows_csv[f]['frame_ms']):.0f} ms  {causes(rows_csv[f])}")

        means = block_means(rows, w, h, a.block)
        events = find_events(means, a.threshold, a.max_len, a.search)
        print(f"  {len(events)} flashing-block events:")
        for c, s, k, m in events[: a.top]:
            ys, xs = np.nonzero(m)
            print(f"    {times[s]:+5.2f} s frames {frames[s]}-{frames[s + k - 1]} ({k}): {c} blocks, "
                  f"rows {ys.min()}-{ys.max()} cols {xs.min()}-{xs.max()}")
            tiles = [tile(rows, s - 1, w, h), outline(tile(rows, s, w, h), m, a.block),
                     outline(tile(rows, s + k - 1, w, h), m, a.block), tile(rows, s + k, w, h)]
            write_png(os.path.join(d, f"event-{frames[s]}-{k}.png"),
                      np.concatenate([np.concatenate(tiles[:2], axis=1), np.concatenate(tiles[2:], axis=1)]))
        jumps, floor = find_jumps(means, a.max_len)
        print(f"  {len(jumps)} whole-image jumps (floor {floor:.1f}):")
        for score, s, k in jumps[: a.top]:
            print(f"    {times[s]:+5.2f} s frames {frames[s]}-{frames[s + k - 1]} ({k}): {score:.1f}x")
            tiles = [tile(rows, s - 1, w, h), tile(rows, s, w, h), tile(rows, s + k - 1, w, h),
                     tile(rows, s + k, w, h)]
            write_png(os.path.join(d, f"jump-{frames[s]}-{k}.png"),
                      np.concatenate([np.concatenate(tiles[:2], axis=1), np.concatenate(tiles[2:], axis=1)]))
        picks = [int(round(i)) for i in np.linspace(0, n - 1, 9)]
        write_png(os.path.join(d, "overview.png"), overview(rows, w, h, picks))
        print("  overview: " + " ".join(f"{times[i]:+.1f}s" for i in picks))


if __name__ == "__main__":
    main()
