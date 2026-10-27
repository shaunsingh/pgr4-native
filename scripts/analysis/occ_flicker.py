#!/usr/bin/env python3
"""Measure the light-flare flicker in an occlusion trace (--pgr4_native_occlusion_trace).

The trace has one line per event: "frame query what z_pass total", with `what` one of
result (what GetData read), retire (what the GPU measured for one frame), end (the pair range),
or why none was written (fence, blocks, read, none). A flare blinks when the visibility it is
delivered (ZPass > 0) changes from one read to the next; --pgr4_native_occlusion_window sums the
newest frames so a blink becomes a ramp (0,0) (1,4) (2,6) (3,8) instead of (0,2) <-> (2,2).

  occ_flicker.py TRACE...           reads, flips per 1000 reads, partial and full reads
  occ_flicker.py TRACE --shapes N   the ratio stream around N flips of the worst query
"""
import argparse
import collections


def load(path):
    reads, retires = collections.defaultdict(list), collections.defaultdict(list)
    for line in open(path):
        p = line.split()
        if len(p) < 5:
            continue
        if p[2] == 'result':
            reads[p[1]].append((int(p[0]), int(p[3]), int(p[4])))
        elif p[2] == 'retire':
            retires[p[1]].append((int(p[0]), int(p[3]), int(p[4])))
    return reads, retires


def flips(rs):
    vis = [z > 0 for _, z, t in rs if t > 0]
    return sum(1 for i in range(1, len(vis)) if vis[i] != vis[i - 1]), len(vis)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('traces', nargs='+')
    ap.add_argument('--shapes', type=int, default=0, metavar='N')
    args = ap.parse_args()
    for path in args.traces:
        reads, retires = load(path)
        rf = rr = n = partial = 0
        for q, rs in reads.items():
            f, m = flips(rs)
            rf += f
            n += m
            partial += sum(1 for _, z, t in rs if 0 < z < t)
        for rs in retires.values():
            rr += flips(rs)[0]
        print(f'{path}: reads={n} delivered flips={rf} ({rf * 1000 / max(n, 1):.2f}/kread), '
              f'underlying flips={rr}, partial-ratio reads={partial}')
        for _ in range(args.shapes):
            q, rs = max(reads.items(), key=lambda kv: flips(kv[1])[0])
            idx = [i for i in range(1, len(rs)) if (rs[i][1] > 0) != (rs[i - 1][1] > 0)]
            print(f'  worst {q}: {len(idx)} flips')
            for i in idx[:4]:
                print('   ', [(z, t) for _, z, t in rs[max(0, i - 3):i + 4]])
            break


main()
