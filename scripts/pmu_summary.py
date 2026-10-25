#!/usr/bin/env python3
"""Per-thread front-end counters from deck-run.sh --pmu (perf stat --per-thread -x, output).

  pmu_summary.py <run>-pmu.csv [--threads 8]

Prints, for the threads with the most user-space cycles: their share of the cycles, instructions
per cycle, the fraction of cycles the instruction fetch stalled, and per 1000 instructions the
instruction cache fills from L2 and from memory and the instruction fetches that missed both TLBs.
"""
import argparse
import collections
import csv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--threads", type=int, default=8)
    a = ap.parse_args()
    counts = collections.defaultdict(dict)
    with open(a.csv) as f:
        for row in csv.reader(f):
            if len(row) < 4 or row[0].startswith("#") or not row[1].strip():
                continue
            try:
                value = float(row[1])
            except ValueError:
                continue  # <not counted>
            counts[row[0].strip()][row[3].split(":")[0]] = value
    total = sum(c.get("cycles", 0) for c in counts.values()) or 1
    print(f"{'thread':28s} cycles  IPC  fetch-stall  IC-L2/ki  IC-mem/ki  iTLB-miss/ki")
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1].get("cycles", 0))[: a.threads]:
        cyc, ins = c.get("cycles", 0), c.get("instructions", 0)
        if not cyc or not ins:
            continue
        ki = ins / 1000
        print(f"{name[:28]:28s} {100 * cyc / total:5.1f}% {ins / cyc:4.2f} "
              f"{100 * c.get('ic_fetch_stall.ic_stall_any', 0) / cyc:9.1f}% "
              f"{c.get('ic_cache_fill_l2', 0) / ki:9.2f} {c.get('ic_cache_fill_sys', 0) / ki:10.2f} "
              f"{c.get('bp_l1_tlb_miss_l2_tlb_miss', 0) / ki:12.3f}")


if __name__ == "__main__":
    main()
