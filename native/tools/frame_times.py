"""Frame-time distribution of a race from the native renderer's per-frame CSV
(--pgr4_native_csv; frame_ms = guest Swap to Swap) and the game log, whose
'PGR4 native: frames N' lines (one per --pgr4_d3d_stats_frames) map wall-clock time to frames.

Averages hide stutter, so this reports what a player feels: the mean, the 1 % and 0.1 % lows
(frame rate over the slowest 1 % / 0.1 % of frames), the 99th percentile and the longest frame,
and the frames that took more than twice the median (hitches).

Two windows:
- 'window': [start, start + seconds] in wall-clock time, from the first frames line after start
  (so it begins up to --pgr4_d3d_stats_frames frames late).
- 'race': a fixed span of game frames, the same in every run of the route. Its anchor is the last
  camera cut (a jump in draws per frame) before the window: the pre-race grid shot, which the
  route's throttle press follows by a constant 8.1-8.3 s. The race starts ~9 s after it, and the
  car's path depends on frame timing, so runs part ways (one may hit a barrier and render far
  less) from ~46 s after it on. With --ref, the per-second median draw counts are checked
  against a reference run's: 'diverged at N s' marks a run whose route differs from N s on.

usage: frame_times.py <frames.csv> <game log> <start HH:MM:SS> [seconds=40] [--ref draws.txt]
       frame_times.py --fingerprint <frames.csv> <game log> <start HH:MM:SS>  (writes a reference)
"""
import csv
import datetime as dt
import re
import statistics
import sys

RACE_FROM_S, RACE_TO_S = 9.0, 45.0  # race window, seconds after the anchor cut


def frames_window(log, start, secs):
    pat = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*PGR4 native: frames (\d+) \|")
    marks = []
    for line in open(log, encoding="utf-8", errors="replace"):
        if m := pat.match(line):
            marks.append((dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f"), int(m.group(2))))
    if not marks:
        sys.exit(f"{log}: no 'PGR4 native: frames' lines")
    t0 = dt.datetime.combine(marks[0][0].date(), dt.datetime.strptime(start, "%H:%M:%S").time())
    t1 = t0 + dt.timedelta(seconds=secs)
    inside = [f for t, f in marks if t0 <= t <= t1]
    if len(inside) < 2:
        sys.exit(f"{log}: fewer than two 'PGR4 native: frames' lines in the window")
    return inside[0], inside[-1]


def race_rows(rows, f0):
    """Rows of the race window and the anchor row, or (None, None) without a cut before f0."""
    anchor = None
    for i in range(1, len(rows)):
        if int(rows[i]["frame"]) >= f0:
            break
        d, p = int(rows[i]["draws"]), int(rows[i - 1]["draws"])
        if abs(d - p) > max(300, 0.25 * p):
            anchor = i
    if anchor is None:
        return None, None
    ta = float(rows[anchor]["t_ms"])
    sel = [r for r in rows[anchor:]
           if RACE_FROM_S * 1000 <= float(r["t_ms"]) - ta < RACE_TO_S * 1000]
    return sel, rows[anchor]


def fingerprint(sel, t_anchor):
    """Median draws per frame in each second of the race window, in hundreds."""
    bins = {}
    for r in sel:
        bins.setdefault(int((float(r["t_ms"]) - t_anchor) / 1000), []).append(int(r["draws"]))
    return [round(statistics.median(bins[s]) / 100) if s in bins else 0
            for s in range(int(RACE_FROM_S), int(RACE_TO_S))]


def diverged_at(fp, ref):
    """First second of two in a row whose draw counts differ from the reference by over 25 %."""
    off = [abs(a - b) > 0.25 * max(b, 4) for a, b in zip(fp, ref)]
    for i in range(len(off) - 1):
        if off[i] and off[i + 1]:
            return int(RACE_FROM_S) + i
    return None


def summary(ms):
    ms = sorted(ms)
    n = len(ms)
    mean = sum(ms) / n
    median = statistics.median(ms)

    def low(fraction):
        worst = ms[-max(1, round(n * fraction)):]
        return 1000.0 / (sum(worst) / len(worst))

    p99 = ms[min(n - 1, int(n * 0.99))]
    hitches = sum(1 for v in ms if v > 2.0 * median)
    return (f"{1000 / mean:.1f} fps avg, 1% low {low(0.01):.1f}, 0.1% low {low(0.001):.1f} | "
            f"frame ms: median {median:.2f}, p99 {p99:.2f}, max {ms[-1]:.2f}, "
            f"sd {statistics.pstdev(ms):.2f} | hitches (>2x median) {hitches}")


def main(argv):
    ref = None
    if "--ref" in argv:
        i = argv.index("--ref")
        ref = argv[i + 1]
        del argv[i:i + 2]
    write_ref = argv[:1] == ["--fingerprint"]
    if write_ref:
        argv = argv[1:]
    path, log, start = argv[:3]
    secs = float(argv[3]) if len(argv) > 3 else 40.0
    f0, f1 = frames_window(log, start, secs)
    rows = list(csv.DictReader(open(path)))
    sel, anchor = race_rows(rows, f0)
    if write_ref:
        if not sel:
            sys.exit(f"{path}: no camera cut before frame {f0}")
        print(" ".join(map(str, fingerprint(sel, float(anchor["t_ms"])))))
        return
    ms = [float(r["frame_ms"]) for r in rows if f0 <= int(r["frame"]) < f1]
    if not ms:
        sys.exit(f"{path}: no rows for frames {f0}-{f1}")
    print(f"{path}: {len(ms)} frames ({f0}-{f1}) -> {summary(ms)}")
    if not sel:
        print(f"race: no camera cut before frame {f0}")
        return
    route = ""
    if ref:
        fp = fingerprint(sel, float(anchor["t_ms"]))
        at = diverged_at(fp, [int(v) for v in open(ref).read().split()])
        route = " | route matches the reference" if at is None else f" | route diverged at {at} s"
    print(f"race +{RACE_FROM_S:.0f}..{RACE_TO_S:.0f} s after the cut at frame {anchor['frame']}: "
          f"{len(sel)} frames ({sel[0]['frame']}-{sel[-1]['frame']}) -> "
          f"{summary([float(r['frame_ms']) for r in sel])}{route}")


main(sys.argv[1:])
