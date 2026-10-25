"""Texture-stage race window of a native CSV (--pgr4_native_profile): medians over steady race frames
(from 300 frames after the race-start frame, the one with the most pso_pending; no PSO created,
uploads under 256 KB, over 1,000 draws), ns/draw per stage, binding counters, whole-run totals.
Usage: perf_race.py <native.csv> [...]"""
import csv, statistics, sys

for path in sys.argv[1:]:
    rows = [r for r in csv.DictReader(open(path)) if r.get("frame_ms")]
    f = lambda r, k: float(r[k]) if r.get(k) not in (None, "") else 0.0
    start = max(range(len(rows)), key=lambda i: f(rows[i], "pso_pending") if "pso_pending" in rows[i] else 0)
    sel = [r for r in rows[start + 300:] if f(r, "pso_created") == 0 and f(r, "upload_kb") < 256 and f(r, "draws") > 1000]
    print(f"{path.split('/')[-1]}: race start f{rows[start]['frame']}, {len(sel)} steady race frames, draws median {statistics.median(f(r,'draws') for r in sel):.0f}")
    med = lambda fn: statistics.median(fn(r) for r in sel)
    nd = lambda k: med(lambda r: 1000 * f(r, k) / f(r, "draws"))
    stages = [k for k in rows[0] if k.startswith("st_")]
    print("  ns/draw: " + " ".join(f"{k[3:-3]}={nd(k):.0f}" for k in stages))
    print(f"  us_per_draw {med(lambda r: f(r,'us_per_draw')):.3f}  textures/frame {med(lambda r: f(r,'textures')):.0f}  textures_missing {med(lambda r: f(r,'textures_missing')):.0f}")
    for k in ["tex_memo_miss", "srv_lookups", "bind_hits", "bind_misses", "bind_hit", "bind_mru", "bind_miss_empty", "bind_miss_fetch",
              "bind_miss_tex", "bind_nomemo", "get_miss_fetch", "get_miss_changed", "srv_memo_hit", "tex_upload_skip",
              "tex_uploads", "tex_resolved_copies", "tex_copy_resolved_us"]:
        if k in rows[0]:
            print(f"  {k:20s} median/frame {med(lambda r: f(r,k)):8.0f}")
    tot = lambda k: sum(f(r, k) for r in rows)
    print(f"  whole run: textures_missing {tot('textures_missing'):.0f} of {tot('textures'):.0f}; uploads {tot('tex_uploads'):.0f} ({tot('tex_upload_kb')/1024:.0f} MB) untile {tot('tex_untile_us')/1000:.0f} ms; upload_skip {tot('tex_upload_skip'):.0f}")
