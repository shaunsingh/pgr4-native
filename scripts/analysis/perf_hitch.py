"""Frames over a threshold in a profiled native CSV, with their slowest stages and counters.

usage: perf_hitch.py <native.csv> [min_ms=50] [min_draws=0]

min_draws: count only frames with at least that many draws. 100 leaves out the title-screen
frames: a few hundred of them take 20-24 ms with 16 draws and would drown the real hitches.
"""
import csv
import sys

path = sys.argv[1]
min_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
rows = [r for r in csv.DictReader(open(path)) if r.get("frame_ms")]
cols = [k for k in rows[0] if k.startswith("st_")]
f = lambda r, k: float(r[k]) if r.get(k) not in (None, "") else 0.0
min_draws = int(sys.argv[3]) if len(sys.argv) > 3 else 0
slow = [r for r in rows if f(r, "frame_ms") >= min_ms and f(r, "draws") >= min_draws]
print(f"{path}: {len(rows)} frames, {len(slow)} at >= {min_ms} ms with >= {min_draws} draws, "
      f"total {sum(f(r,'frame_ms') for r in slow)/1000:.2f} s")
pending = [r for r in rows if f(r, "pso_pending") > 0]
print(f"pso_pending: {sum(f(r, 'pso_pending') for r in rows):.0f} draws in {len(pending)} frames; "
      f"pso created: {sum(f(r, 'pso_created') for r in rows):.0f}")
for r in slow:
    top = sorted(((f(r, k), k) for k in cols), reverse=True)[:3]
    extra = ""
    for k in ("tex_created", "tex_create_us", "tex_uploads", "tex_upload_us", "tex_untile_us", "pso_pending"):
        if k in r:
            extra += f" {k}={r[k]}"
    print(f"  f{r['frame']} {f(r,'frame_ms'):8.1f} ms draws {r['draws']} pso {r['pso_created']} draw_us {f(r,'draw_us'):8.0f} "
          + " ".join(f"{k[3:-3]}={v/1000:.1f}ms" for v, k in top) + extra)
