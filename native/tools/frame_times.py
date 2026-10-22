"""Frame-time distribution of a race window from the native renderer's per-frame CSV
(--pgr4_native_csv; frame_ms = guest Swap to Swap) and the game log, whose
'PGR4 native: frames N' lines (one per --pgr4_d3d_stats_frames) map wall-clock time to frames.

Averages hide stutter, so this reports what a player feels: the mean, the 1 % and 0.1 % lows
(frame rate over the slowest 1 % / 0.1 % of frames), the 99th percentile and the longest frame,
and the frames that took more than twice the median (hitches).

usage: frame_times.py <frames.csv> <game log> <start HH:MM:SS> [seconds=40]
"""
import csv
import datetime as dt
import re
import statistics
import sys

path, log, start = sys.argv[1:4]
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 40.0
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
f0, f1 = inside[0], inside[-1]
ms = [float(r["frame_ms"]) for r in csv.DictReader(open(path)) if f0 <= int(r["frame"]) < f1]
if not ms:
    sys.exit(f"{path}: no rows for frames {f0}-{f1}")

ms.sort()
n = len(ms)
mean = sum(ms) / n
median = statistics.median(ms)


def low(fraction):
    worst = ms[-max(1, round(n * fraction)):]
    return 1000.0 / (sum(worst) / len(worst))


p99 = ms[min(n - 1, int(n * 0.99))]
hitches = sum(1 for v in ms if v > 2.0 * median)
print(f"{path}: {n} frames ({f0}-{f1}) -> {1000 / mean:.1f} fps avg, 1% low {low(0.01):.1f}, "
      f"0.1% low {low(0.001):.1f} | frame ms: median {median:.2f}, p99 {p99:.2f}, max {ms[-1]:.2f}, "
      f"sd {statistics.pstdev(ms):.2f} | hitches (>2x median) {hitches}")
