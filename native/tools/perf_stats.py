"""Summarize a native CSV (--pgr4_native_profile): stage cost over the last N frames."""
import csv
import statistics
import sys

path = sys.argv[1]
last = int(sys.argv[2]) if len(sys.argv) > 2 else 600
rows = list(csv.DictReader(open(path)))
rows = [r for r in rows if r.get("frame_ms")]
sel = rows[-last:]
f = lambda r, k: float(r[k]) if r.get(k) not in (None, "") else 0.0
cols = [k for k in rows[0].keys() if k.startswith("st_")]
NESTED = {"st_cap_watch_us", "st_upload_list_us", "st_guest_d3d_us"}
print(f"{path}: {len(rows)} rows, using last {len(sel)}")
n = len(sel)
draws = sum(f(r, "draws") for r in sel) / n
print(f"frame_ms median {statistics.median(f(r,'frame_ms') for r in sel):.2f} mean {sum(f(r,'frame_ms') for r in sel)/n:.2f}")
for k in ["draws", "clears", "resolves", "draw_us", "clear_us", "resolve_us", "swap_us", "pso_created", "upload_kb", "textures", "track_calls",
          "tex_memo_miss", "tex_created", "tex_create_us", "tex_uploads", "tex_deferred", "tex_upload_kb", "tex_upload_us", "tex_untile_us", "tex_record_us",
          "tex_resolved_copies", "tex_copy_resolved_us", "srv_lookups", "srv_created", "samplers_created",
          "bind_hits", "bind_misses", "bind_hit", "bind_mru", "bind_miss_empty", "bind_miss_fetch", "bind_miss_tex",
          "bind_nomemo", "get_miss_fetch", "get_miss_changed", "srv_memo_hit", "tex_upload_skip"]:
    if k in sel[0]:
        vals = [f(r, k) for r in sel]
        print(f"  {k:14s} mean {sum(vals)/n:10.1f}  median {statistics.median(vals):10.1f}  max {max(vals):10.1f}")
print("stage            mean_us/frame  median_us/frame  ns/draw(mean)")
tot = 0.0
for k in cols:
    vals = [f(r, k) for r in sel]
    m = sum(vals) / n
    # Nested timers: st_tex_* in st_textures, st_cap_watch in st_cap_vertices, st_upload_list in
    # st_submit or the swap; st_guest_d3d overlaps st_track (hook side).
    if not k.startswith("st_tex_") and k not in NESTED:
        tot += m
    print(f"  {k:22s} {m:10.1f} {statistics.median(vals):10.1f} {1000*m/max(draws,1):10.0f}")
print(f"  sum of stages {tot:.1f} us/frame")
# frames with no new PSO and small uploads: steady-state cost
steady = [r for r in sel if f(r, "pso_created") == 0 and f(r, "upload_kb") < 256]
if steady:
    print(f"steady frames (no PSO, upload<256KB): {len(steady)}  frame_ms median {statistics.median(f(r,'frame_ms') for r in steady):.2f}  draw_us median {statistics.median(f(r,'draw_us') for r in steady):.0f}")
    for k in cols:
        nsd = statistics.median(1000 * f(r, k) / max(f(r, "draws"), 1) for r in steady)
        print(f"  {k:22s} median {statistics.median(f(r,k) for r in steady):10.1f}  ns/draw {nsd:6.0f}")
print("worst 12 frames:")
for r in sorted(sel, key=lambda r: -f(r, "frame_ms"))[:12]:
    top = sorted(((f(r, k), k) for k in cols), reverse=True)[:3]
    print(f"  f{r['frame']} {f(r,'frame_ms'):7.1f} ms draws {r['draws']} pso {r['pso_created']} up {f(r,'upload_kb'):8.0f}KB  " +
          " ".join(f"{k[3:-3]}={v/1000:.1f}ms" for v, k in top))
