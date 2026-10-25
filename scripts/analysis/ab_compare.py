"""Paired A/B comparison of race runs: frame time in matched time bins after the race mark.

Each run's game log has a "PGR4 D3D: frames N | per frame: ... tap draws D" line every 150 frames.
The race route is replayed by time, so the scene at t seconds after "mark race" is about the same
in every run. Comparing bin by bin (same scene) removes most of the scene-to-scene noise that
made whole-window averages differ by ~8 % between identical runs.

input: a CSV with columns side,log,mark[,tag,compilers] (side = A or B; log = the game log;
mark = ISO timestamp of the input player's "mark race"). Runs should alternate A,B,B,A,...
usage: ab_compare.py <runs.csv> [--seconds 36] [--bin 4]
"""
import argparse
import csv
import datetime as dt
import math
import re
import statistics

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*PGR4 D3D: frames (\d+) \| per frame: calls \d+, draw calls [\d.]+, tap draws ([\d.]+)")


def windows(log: str):
    out = []
    for line in open(log, encoding="utf-8", errors="replace"):
        m = LINE.match(line)
        if m:
            out.append((dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f"), int(m.group(2)), float(m.group(3))))
    return out


def bins_for(log: str, mark: dt.datetime, seconds: float, width: float):
    """Per bin: (frames, seconds, draw-weighted frames) from the windows that end inside it."""
    rows = windows(log)
    nb = int(seconds // width)
    acc = [[0, 0.0, 0.0] for _ in range(nb)]
    for a, b in zip(rows, rows[1:]):
        t0 = (a[0] - mark).total_seconds()
        t1 = (b[0] - mark).total_seconds()
        if t0 < 0 or t1 > nb * width:
            continue
        k = min(int(((t0 + t1) / 2) // width), nb - 1)
        n = b[1] - a[1]
        acc[k][0] += n
        acc[k][1] += t1 - t0
        acc[k][2] += b[2] * n
    return [(f, s, d) for f, s, d in acc]


def tcrit(df: int) -> float:
    table = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 12: 2.18, 15: 2.13, 20: 2.09}
    return table.get(df, 2.0 if df > 20 else table[min(table, key=lambda k: abs(k - df))])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--seconds", type=float, default=36)
    ap.add_argument("--bin", type=float, default=4)
    args = ap.parse_args()
    runs = list(csv.DictReader(open(args.runs)))
    nb = int(args.seconds // args.bin)
    per_side = {"A": [], "B": []}
    print(f"{'run':8} {'side':4} {'ms/frame':>9} {'fps':>7} {'draws':>7} compilers  log")
    for r in runs:
        # PowerShell writes 7 fractional digits and an offset; the game log is local time.
        mark = dt.datetime.fromisoformat(re.sub(r"(\.\d{6})\d*", r"\1", r["mark"])[:26])
        b = bins_for(r["log"], mark, args.seconds, args.bin)
        frames = sum(x[0] for x in b)
        secs = sum(x[1] for x in b)
        draws = sum(x[2] for x in b) / max(frames, 1)
        ms = 1000 * secs / max(frames, 1)
        per_side[r["side"]].append(b)
        flag = "" if r.get("compilers", "0 / 0").strip() in ("0 / 0", "", "0") else "  <- compilers, inflated"
        print(f"{r.get('tag', ''):8} {r['side']:4} {ms:9.2f} {1000 / ms if ms else 0:7.1f} {draws:7.0f} {r.get('compilers', ''):9}{flag}  {r['log']}")

    def side_bins(side):
        """Mean ms/frame and draws per bin over the side's runs (frame-weighted)."""
        out = []
        for k in range(nb):
            f = sum(run[k][0] for run in per_side[side])
            s = sum(run[k][1] for run in per_side[side])
            d = sum(run[k][2] for run in per_side[side])
            out.append((1000 * s / f if f else math.nan, d / f if f else math.nan))
        return out

    if not per_side["A"] or not per_side["B"]:
        print("need runs on both sides")
        return
    a, b = side_bins("A"), side_bins("B")
    print(f"\nbin (s after mark)   A ms/frame  B ms/frame   B-A     A draws  B draws")
    diffs = []
    for k in range(nb):
        if math.isnan(a[k][0]) or math.isnan(b[k][0]):
            continue
        d = b[k][0] - a[k][0]
        diffs.append((d, a[k][0]))
        print(f"  {k * args.bin:4.0f}-{(k + 1) * args.bin:<4.0f}         {a[k][0]:9.2f}  {b[k][0]:9.2f}  {d:+7.2f}  {a[k][1]:8.0f} {b[k][1]:8.0f}")
    if len(diffs) < 2:
        print("too few bins")
        return
    rel = [d / base * 100 for d, base in diffs]
    mean, sd = statistics.mean(rel), statistics.stdev(rel)
    half = tcrit(len(rel) - 1) * sd / math.sqrt(len(rel))
    ma = statistics.mean(x for x, _ in a if not math.isnan(x))
    mb = statistics.mean(x for x, _ in b if not math.isnan(x))
    print(f"\nA {ma:.2f} ms/frame ({1000 / ma:.1f} fps), B {mb:.2f} ms/frame ({1000 / mb:.1f} fps)")
    print(f"B vs A, paired over {len(rel)} bins: {mean:+.1f} % frame time (95 % CI {mean - half:+.1f} to {mean + half:+.1f} %)")
    verdict = "B faster" if mean + half < 0 else "B slower" if mean - half > 0 else "no clear difference"
    print(f"verdict: {verdict}")


if __name__ == "__main__":
    main()
