#!/usr/bin/env python3
"""Summarises a pgr4-d3d-trace file (written by src/pgr4_d3d_trace.cpp) and checks it.

  trace_summary.py pgr4-d3d-race-f12345.txt [...]

Checks:
- every draw packet is attributed to a hooked function (no "fn=-" draws);
- predicated draw packets outside Resolve == calls of DrawCounter's five draw builders (DrawIndexedVertices,
  DrawVertices, BeginVertices, DrawIndx2Inline, DrawIndx2Split);
- the `# summary` counters agree with the D lines;
- the decoded registers matched the device's register shadow at every draw (lists exceptions).
"""
import collections
import re
import sys

DRAW_BUILDERS = {'DrawIndexedVertices', 'DrawVertices', 'BeginVertices', 'DrawIndx2Inline', 'DrawIndx2Split'}


def summarise(path):
    draws = collections.Counter()      # emitting function -> predicated draws
    aux = collections.Counter()
    resolves = 0
    calls = collections.Counter()
    shader_pairs = collections.Counter()
    rts = collections.Counter()
    tex = set()
    unattributed = 0
    summary = {}
    mismatch = []
    header = ''
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('# pgr4-d3d-trace'):
            header = line.strip()
        elif line.startswith('C '):
            calls[line.split()[3]] += 1
        elif line.startswith('D '):
            f = dict(kv.split('=', 1) for kv in line.split()[2:] if '=' in kv)
            fn = f.get('fn', '-')
            if fn == '-':
                unattributed += 1
            if 'RESOLVE' in line.split():
                resolves += 1
            if f.get('pred') == '1':
                draws[fn] += 1
            else:
                aux[fn] += 1
            if 'vs' in f:
                shader_pairs[(f['vs'].split(':')[-1], f['ps'].split(':')[-1])] += 1
                rts[(f['c0'], f['d'], f['mode'])] += 1
            for k, v in f.items():
                if re.fullmatch(r't\d+', k):
                    tex.add(v)
        elif line.startswith('# summary'):
            summary = dict(kv.split('=', 1) for kv in line.split()[2:])
        elif line.startswith(('# state_mismatch', '# shadow_mismatch')):
            mismatch.append(line.split(None, 2)[2].strip())
    pred = sum(draws.values())
    builder_calls = sum(n for name, n in calls.items() if name in DRAW_BUILDERS)
    print(f'{path}\n  {header}')
    print(f'  hooked calls {sum(calls.values())}, draw-builder calls {builder_calls}')
    print(f'  draw packets: predicated {pred}, other {sum(aux.values())} (resolves {resolves}), unattributed {unattributed}')
    print('  predicated draws by function: ' + ', '.join(f'{k} {v}' for k, v in draws.most_common()))
    print('  other draws by function: ' + ', '.join(f'{k} {v}' for k, v in aux.most_common()))
    print(f'  shader pairs {len(shader_pairs)}, textures {len(tex)}, (c0, depth, mode) combinations {len(rts)}')
    for (c0, d, mode), n in rts.most_common(8):
        print(f'    c0={c0} d={d} mode={mode}: {n} draws')
    print('  summary line: ' + ' '.join(f'{k}={v}' for k, v in summary.items()))
    ok = True
    for name, cond in [
        ('no unattributed draws', unattributed == 0),
        # Resolve emits a few predicated rectangles of its own (per-tile clears); every other
        # predicated draw packet comes from one draw-builder call.
        ('predicated draws outside Resolve == draw-builder calls', pred - draws.get('Resolve', 0) == builder_calls),
        ('summary tap_draws == D lines', str(pred) == summary.get('tap_draws')),
        ('summary tap_resolves == D lines', str(resolves) == summary.get('tap_resolves')),
        ('no resyncs', summary.get('resyncs') == '0'),
        ('no torn packets', summary.get('torn') == '0'),
        ('no unknown opcodes', summary.get('unknown_ops') == '0'),
    ] + ([
        # v2 traces: d3d_state's GPU view equals the decoded stream.
        ('d3d_state == tap at every draw', summary.get('state_mismatch_draws') == '0'
         and summary.get('state_unchecked') == '0'),
    ] if 'state_checked_draws' in summary else []):
        print(f'  [{"ok" if cond else "FAIL"}] {name}')
        ok &= cond
    if 'state_checked_draws' in summary:
        print(f'  d3d_state vs tap: {summary["state_checked_draws"]} draws checked, '
              f'{summary["state_mismatch_draws"]} differ ({summary["state_shader_mismatch"]} in shaders), '
              f'{summary["state_unchecked"]} unchecked, {summary.get("state_per_tile", "?")} per-tile '
              'register values skipped; registers: ' + ('; '.join(mismatch) if mismatch else 'none'))
    else:
        print(f'  shadow mismatches: {summary.get("shadow_mismatch_draws", "?")} draws; registers: ' +
              ('; '.join(mismatch) if mismatch else 'none'))
    return ok


if __name__ == '__main__':
    results = [summarise(p) for p in sys.argv[1:]]
    sys.exit(0 if results and all(results) else 1)
