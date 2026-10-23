#!/usr/bin/env python3
"""Find the frames of a glitch catcher dump (.rec) that flash, and write them out as PNGs.

A flash is a stretch of 1 to --max-len frames in which part of the image departs from what it
showed just before and returns to it just after. The image is cut into --block x --block pixel
blocks; a block flashes when its mean colour over the stretch is at least --threshold (0-255, any
channel) away from every block within --search blocks of it in the frame before and in the frame
after (so that motion, which moves a block's content to a nearby block, does not count), the frames
before and after match each other within a third of that, and the stretch's own frames within
half of it. Events are ranked by how many blocks flash together and written as 2x2 sheets
(before, first and last frame of the stretch with its blocks outlined, after).

  flicker.py <rec> [--csv frames.csv] [--out DIR] [--top 16] [--block 10] [--search 2]
             [--threshold 40] [--max-len 4] [--sheet FROM TO STEP] [--jumps]

--csv adds the frame CSV's columns (tex_stale, upload_kb, ...) of each event's frames.
--sheet writes contact sheets of every STEP-th frame with frame index in [FROM, TO] instead.
--jumps finds whole-image glitches instead: 1 to --max-len frames whose mean block difference
from the frame before is at least 6x the recording's median (and 15) and that return to it after.
Needs numpy.
"""

import argparse
import csv
import os
import struct
import sys
import zlib

import numpy as np

HEADER = struct.Struct("<4sIII QQ")


def open_rec(path):
    with open(path, "rb") as f:
        magic, w, h, _, _, _ = HEADER.unpack(f.read(HEADER.size))
    if magic != b"PGRF":
        sys.exit(f"{path}: not a recording")
    size = HEADER.size + w * h * 3
    n = os.path.getsize(path) // size
    mm = np.memmap(path, dtype=np.uint8, mode="r", shape=(n, size))
    meta = np.frombuffer(mm[:, :HEADER.size].tobytes(), dtype=np.uint64).reshape(n, 4)
    return mm, w, h, meta[:, 2].astype(np.int64), meta[:, 3].astype(np.int64)


def pixels(mm, i, w, h):
    return np.asarray(mm[i, HEADER.size:]).reshape(h, w, 3)


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = np.zeros((h, w * 3 + 1), dtype=np.uint8)
    raw[:, 1:] = rgb.reshape(h, w * 3)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw.tobytes(), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def block_means(mm, w, h, b):
    by, bx = h // b, w // b
    n = mm.shape[0]
    out = np.empty((n, by, bx, 3), dtype=np.float32)
    for i in range(0, n, 128):
        j = min(n, i + 128)
        p = np.asarray(mm[i:j, HEADER.size:]).reshape(j - i, h, w, 3)[:, : by * b, : bx * b]
        out[i:j] = p.reshape(j - i, by, b, bx, b, 3).mean(axis=(2, 4), dtype=np.float32)
    return out


def nearest(a, b, search):
    """Per block of a, the smallest max-channel difference to b's blocks within search blocks."""
    by, bx = a.shape[1:3]
    pad = np.pad(b, ((0, 0), (search, search), (search, search), (0, 0)), mode="edge")
    best = None
    for dy in range(2 * search + 1):
        for dx in range(2 * search + 1):
            d = np.abs(a - pad[:, dy : dy + by, dx : dx + bx]).max(axis=-1)
            best = d if best is None else np.minimum(best, d)
    return best


def find_events(means, threshold, max_len, search):
    """(flashing blocks, start, length, block mask), the best length per start, overlaps merged."""
    n = means.shape[0]
    csum = np.concatenate([np.zeros_like(means[:1]), np.cumsum(means, axis=0, dtype=np.float64)])
    best = {}
    for k in range(1, max_len + 1):
        if n < k + 2:
            break
        before = means[: n - k - 1]
        after = means[k + 1 :]
        during = ((csum[k + 1 : n] - csum[1 : n - k]) / k).astype(np.float32)
        dev = np.minimum(nearest(during, before, search), nearest(during, after, search))
        ret = nearest(before, after, search)
        mask = (dev >= threshold) & (ret * 3 < dev)
        if k > 1:
            # A flash holds still over its frames; motion through the block does not.
            win = np.lib.stride_tricks.sliding_window_view(means[1 : n - 1], k, axis=0)
            spread = (win.max(axis=-1) - win.min(axis=-1)).max(axis=-1)
            mask &= spread * 2 < dev
        counts = mask.sum(axis=(1, 2))
        for t in np.nonzero(counts)[0]:
            start = int(t) + 1
            c = int(counts[t])
            if c > best.get(start, (0,))[0]:
                best[start] = (c, k, mask[t])
    events = sorted(((c, s, k, m) for s, (c, k, m) in best.items()), key=lambda e: -e[0])
    taken = []
    for c, s, k, m in events:
        if all(s + k + 2 <= s2 or s2 + k2 + 2 <= s for _, s2, k2, _ in taken):
            taken.append((c, s, k, m))
    return taken


def find_jumps(means, max_len):
    """(score, start, length): stretches that jump away from the frame before and come back."""
    n = means.shape[0]
    flat = means.reshape(n, -1)
    step = np.abs(flat[1:] - flat[:-1]).mean(axis=1)
    floor = max(15.0, 6.0 * float(np.median(step)))
    out = []
    i = 1
    while i < n:
        d = float(step[i - 1])
        if d < floor:
            i += 1
            continue
        for k in range(1, max_len + 1):
            if i + k >= n:
                break
            back = float(np.abs(flat[i + k] - flat[i - 1]).mean())
            if back * 2 < d:
                out.append((d / floor, i, k))
                i += k
                break
        i += 1
    return sorted(out, key=lambda e: -e[0]), floor


def outline(img, mask, b):
    img = img.copy()
    for y, x in zip(*np.nonzero(mask)):
        y0, x0 = y * b, x * b
        img[y0, x0 : x0 + b] = img[y0 + b - 1, x0 : x0 + b] = (255, 0, 0)
        img[y0 : y0 + b, x0] = img[y0 : y0 + b, x0 + b - 1] = (255, 0, 0)
    return img


def load_csv(path):
    with open(path) as f:
        return {int(r["frame"]): r for r in csv.DictReader(f)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("rec")
    ap.add_argument("--csv")
    ap.add_argument("--out")
    ap.add_argument("--top", type=int, default=16)
    ap.add_argument("--block", type=int, default=10)
    ap.add_argument("--search", type=int, default=2)
    ap.add_argument("--threshold", type=float, default=40.0)
    ap.add_argument("--max-len", type=int, default=4)
    ap.add_argument("--sheet", type=int, nargs=3, metavar=("FROM", "TO", "STEP"))
    ap.add_argument("--jumps", action="store_true")
    a = ap.parse_args()
    mm, w, h, frames, times = open_rec(a.rec)
    out = a.out or os.path.splitext(a.rec)[0] + "-flicker"
    os.makedirs(out, exist_ok=True)
    print(f"{a.rec}: {len(frames)} frames {w}x{h}, frame {frames[0]}-{frames[-1]}, "
          f"{(times[-1] - times[0]) / 1e6:.1f} s")
    index = {int(f): i for i, f in enumerate(frames)}

    if a.sheet:
        lo, hi, step = a.sheet
        picks = [index[f] for f in range(lo, hi + 1, step) if f in index]
        tw, th = w // 2, h // 2
        for s in range(0, len(picks), 9):
            group = picks[s : s + 9]
            sheet = np.zeros((th * 3, tw * 3, 3), dtype=np.uint8)
            for j, i in enumerate(group):
                p = pixels(mm, i, w, h)[: th * 2, : tw * 2]
                small = p.reshape(th, 2, tw, 2, 3).mean(axis=(1, 3)).astype(np.uint8)
                sheet[(j // 3) * th : (j // 3 + 1) * th, (j % 3) * tw : (j % 3 + 1) * tw] = small
            path = os.path.join(out, f"sheet-{frames[group[0]]}-{frames[group[-1]]}.png")
            write_png(path, sheet)
            print(path, "frames", " ".join(str(frames[i]) for i in group))
        return

    means = block_means(mm, w, h, a.block)
    rows = load_csv(a.csv) if a.csv else {}
    cols = ["tex_stale", "upload_kb", "textures_missing", "pso_pending", "held", "frame_ms"]
    if a.jumps:
        cols += ["watch_arm_pages", "hash_pages", "swap_fence_us", "coh_stale", "coh_late"]
        jumps, floor = find_jumps(means, a.max_len)
        print(f"{len(jumps)} jumps (floor {floor:.1f}); top {min(a.top, len(jumps))}:")
        for score, s, k in jumps[: a.top]:
            f0, f1 = int(frames[s]), int(frames[s + k - 1])
            extra = ""
            if rows:
                vals = [rows.get(f) for f in range(f0, f1 + 1)]
                extra = " | " + " ".join(
                    f"{col} " + "/".join(v.get(col, "?") if v else "?" for v in vals) for col in cols)
            print(f"  frames {f0}-{f1} ({k}) at {(times[s] - times[0]) / 1e6:.2f} s: "
                  f"{score:.1f}x{extra}")
            tiles = [pixels(mm, s - 1, w, h), pixels(mm, s, w, h), pixels(mm, s + k - 1, w, h),
                     pixels(mm, s + k, w, h)]
            sheet = np.concatenate([np.concatenate(tiles[:2], axis=1),
                                    np.concatenate(tiles[2:], axis=1)])
            write_png(os.path.join(out, f"jump-{f0}-{k}.png"), sheet)
        print(f"sheets in {out}")
        return
    events = find_events(means, a.threshold, a.max_len, a.search)
    print(f"{len(events)} events; top {min(a.top, len(events))} by flashing blocks "
          f"(of {means.shape[1] * means.shape[2]}):")
    for c, s, k, m in events[: a.top]:
        f0, f1 = int(frames[s]), int(frames[s + k - 1])
        t = (times[s] - times[0]) / 1e6
        extra = ""
        if rows:
            vals = [rows.get(f) for f in range(f0, f1 + 1)]
            extra = " | " + " ".join(
                f"{col} " + "/".join(v[col] if v else "?" for v in vals) for col in cols)
        ys, xs = np.nonzero(m)
        print(f"  frames {f0}-{f1} ({k}) at {t:.2f} s: {c} blocks, rows {ys.min()}-{ys.max()} "
              f"cols {xs.min()}-{xs.max()}{extra}")
        tiles = [pixels(mm, s - 1, w, h), outline(pixels(mm, s, w, h), m, a.block),
                 outline(pixels(mm, s + k - 1, w, h), m, a.block), pixels(mm, s + k, w, h)]
        sheet = np.concatenate([np.concatenate(tiles[:2], axis=1), np.concatenate(tiles[2:], axis=1)])
        write_png(os.path.join(out, f"event-{f0}-{k}.png"), sheet)
    print(f"sheets in {out}")


if __name__ == "__main__":
    main()
