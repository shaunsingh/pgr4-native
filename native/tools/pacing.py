"""Frame pacing of a race from the native renderer's per-frame CSV (--pgr4_native_csv) and the
frame pacer's present log beside it (<csv stem>-presents.csv), in the window frame_times.py uses.

- steps: the intervals between the game thread beginning its frames (guest_begin_ms), the time
  steps of the game's simulation (PGR4 advances it by measured time).
- shown: the intervals between the presents the display completed (the pacer's log: when the
  swapchain's frame latency waitable was released, vkd3d-proton's vkWaitForPresentKHR return).
- latency: a frame's beginning to the display showing it. Its spread is what the eye sees as
  judder: motion looks even when every frame is shown the same time after the game sampled it,
  whatever the frame rate.
- mismatch: per shown frame, its time step minus the interval the display showed it after the
  frame before; near zero when the steps follow the display.
- waited: from the GPU finishing a frame (its fence, as the pacer saw it) to the display showing
  it, and the delay the latency control began the frame with.

usage: pacing.py <frames.csv> <game log> <start HH:MM:SS> [seconds=40]
"""
import csv
import os
import statistics
import sys

from frame_times import frames_window


def spread(values):
    v = sorted(values)
    n = len(v)
    pct = lambda p: v[min(n - 1, int(n * p))]
    return (f"median {statistics.median(v):.2f}, p1 {pct(0.01):.2f}, p99 {pct(0.99):.2f}, "
            f"sd {statistics.pstdev(v):.2f}")


def main(argv):
    path, log, start = argv[:3]
    secs = float(argv[3]) if len(argv) > 3 else 40.0
    f0, f1 = frames_window(log, start, secs)
    rows = [r for r in csv.DictReader(open(path)) if f0 <= int(r["frame"]) < f1]
    if len(rows) < 10 or "guest_begin_ms" not in rows[0]:
        sys.exit(f"{path}: no pacing columns or too few rows for frames {f0}-{f1}")
    print(f"{path}: frames {f0}-{f1}")
    begin = [float(r["guest_begin_ms"]) for r in rows]
    steps = [b - a for a, b in zip(begin, begin[1:])]
    waits = [float(r["pace_wait_us"]) / 1000 for r in rows]
    print(f"steps: {1000 / statistics.mean(steps):.1f} fps, ms {spread(steps)}; "
          f"pacer wait ms mean {statistics.mean(waits):.2f}")
    presents_path = os.path.splitext(path)[0] + "-presents.csv"
    if not os.path.exists(presents_path):
        print(f"shown: no {os.path.basename(presents_path)} (pacer off)")
        return
    log_rows = {int(r["present"]): r for r in csv.DictReader(open(presents_path))}
    # (frame begin, present log row) per presented frame of the window, in present order.
    presented = [(float(r["guest_begin_ms"]), log_rows[p]) for r in rows
                 if (p := int(r["present"])) in log_rows]
    shown = [(b, float(r["shown_ms"])) for b, r in presented]
    if len(shown) < 10:
        print("shown: too few presents in the window")
        return
    intervals = [b[1] - a[1] for a, b in zip(shown, shown[1:])]
    med = statistics.median(intervals)
    late = sum(1 for d in intervals if d > 1.5 * med)
    print(f"shown: {1000 / statistics.mean(intervals):.1f} fps, interval ms {spread(intervals)}; "
          f"> 1.5x median: {late} of {len(intervals)}")
    print(f"latency ms (frame begin to shown): {spread([s - b for b, s in shown])}")
    mism = [(b[0] - a[0]) - (b[1] - a[1]) for a, b in zip(shown, shown[1:])]
    big = sum(1 for m in mism if abs(m) > 4.0)
    print(f"mismatch ms (time step - shown interval): {spread(mism)}; |x| > 4 ms: {big} of "
          f"{len(mism)}")
    if "ready_ms" in presented[0][1]:
        waited = [float(r["shown_ms"]) - float(r["ready_ms"]) for _, r in presented]
        delay = [float(r["delay_ms"]) for _, r in presented]
        print(f"waited ms (GPU finished to shown): {spread(waited)}; delay ms: {spread(delay)}")


if __name__ == "__main__":
    main(sys.argv[1:])
