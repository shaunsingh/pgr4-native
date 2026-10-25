#!/usr/bin/env python3
"""Lists the render-target passes of a pgr4_d3d_trace frame: which EDRAM surfaces each run of
draws renders to, the Clear and Resolve calls between them, and how EDRAM bases alias across
passes.

  rt_passes.py pgr4-d3d-menu-f2758.txt [--resolves-csv out.csv]
"""
import argparse, collections, re

COLOR_FMT = {0: '8888', 1: '8888_GAMMA', 2: '2_10_10_10', 3: '2_10_10_10_FLOAT', 4: '16_16',
             5: '16_16_16_16', 6: '16_16_FLOAT', 7: '16_16_16_16_FLOAT', 10: '2_10_10_10_AS_10_10_10_10',
             12: '2_10_10_10_FLOAT_AS_16_16_16_16', 14: '32_FLOAT', 15: '32_32_FLOAT'}
DEPTH_FMT = {0: 'D24S8', 1: 'D24FS8'}
TEX_FMT = {2: 'k_8', 4: 'k_5_6_5', 6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 10: 'k_8_8', 25: 'k_16_16',
           26: 'k_16_16_16_16', 30: 'k_16_FLOAT', 31: 'k_16_16_FLOAT', 32: 'k_16_16_16_16_FLOAT',
           36: 'k_32_FLOAT', 37: 'k_32_32_FLOAT', 38: 'k_32_32_32_32_FLOAT'}
MSAA = {0: '1x', 1: '2x', 2: '4x'}
BPP = {0: 4, 1: 4, 2: 4, 3: 4, 4: 4, 5: 8, 6: 4, 7: 8, 10: 4, 12: 4, 14: 4, 15: 8}


def kv(line):
    return dict(m.groups() for m in re.finditer(r'(\w+)=([0-9A-Fa-fx:/,.\w-]+)', line))


def surf_desc(surf, info, depth):
    pitch, msaa = surf & 0x3FFF, (surf >> 16) & 3
    base = info & 0xFFF
    if depth:
        return f'D base={base} {DEPTH_FMT.get((info >> 16) & 1)} pitch={pitch} {MSAA[msaa]}'
    fmt = (info >> 16) & 0xF
    bias = ((info >> 20) & 0x3F)
    bias = bias - 64 if bias & 32 else bias
    return f'C base={base} {COLOR_FMT.get(fmt, fmt)} pitch={pitch} {MSAA[msaa]}' + (f' bias={bias}' if bias else '')


def tiles(surf, info, depth, rows):
    """EDRAM tiles (80x16 samples, 4 KiB) a surface of `rows` pixel rows covers."""
    pitch, msaa = surf & 0x3FFF, (surf >> 16) & 3
    sx = 2 if msaa == 2 else 1
    sy = 2 if msaa >= 1 else 1
    bpp = 4 if depth else BPP.get((info >> 16) & 0xF, 4)
    tiles_x = -(-pitch * sx * (bpp // 4) // 80)
    return tiles_x * -(-rows * sy // 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--resolves-csv')
    a = ap.parse_args()

    calls = {}
    events = []  # ('draw', key, line) / ('clear', args) / ('resolve', fields, call)
    cur_call = None
    top_fn = ''
    with open(a.trace, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('C '):
                p = line.split()
                cur_call = {'seq': int(p[1]), 'depth': int(p[2]), 'fn': p[3], **kv(line)}
                calls[cur_call['seq']] = cur_call
                if cur_call['depth'] == 0:
                    top_fn = p[3]
                if p[3] in ('Clear', 'ClearF') and cur_call['depth'] == 0:
                    events.append(('clear', cur_call))
            elif line.startswith('D '):
                d = kv(line)
                c = calls.get(int(d['c']))
                if ' RESOLVE ' in line:
                    events.append(('resolve', d, c))
                else:
                    # The D3D's own clear quads (inside Clear/ClearF) are not game draws.
                    if 'mode' not in d or top_fn in ('Clear', 'ClearF', 'Resolve'):
                        continue
                    key = (int(d['surf'], 16), int(d['c0'], 16), int(d['c1'], 16), int(d['c2'], 16),
                           int(d['c3'], 16), int(d['d'], 16), int(d['mask'], 16), int(d['depth'], 16),
                           int(d['mode'], 16))
                    events.append(('draw', key, d))

    # Collapse runs of draws with the same bound surfaces into passes.
    passes = []
    for ev in events:
        if ev[0] == 'draw':
            surf, c0, c1, c2, c3, dinfo, mask, depthctl, mode = ev[1]
            sc = ev[2].get('sc', '').split(',')
            rk = (surf, c0, dinfo, (mask >> 4) & 0xFFF and (c1, c2, c3))
            if passes and passes[-1][0] == 'pass' and passes[-1][1] == rk:
                p = passes[-1][2]
            else:
                p = {'draws': 0, 'color': False, 'mrt': set(), 'zread': False, 'zwrite': False, 'stencil': False, 'scmax': (0, 0)}
                passes.append(('pass', rk, p))
            p['draws'] += 1
            if mask & 0xF: p['color'] = True
            for i in (1, 2, 3):
                if (mask >> (4 * i)) & 0xF: p['mrt'].add((i, (c1, c2, c3)[i - 1]))
            if depthctl & 2: p['zread'] = True
            if depthctl & 6 == 6: p['zwrite'] = True
            if depthctl & 1: p['stencil'] = True
            if len(sc) == 2:
                br = int(sc[1], 16)
                p['scmax'] = (max(p['scmax'][0], br & 0x7FFF), max(p['scmax'][1], (br >> 16) & 0x7FFF))
        else:
            passes.append(ev)

    # EDRAM aliasing: which surface (host key) last wrote each EDRAM tile range, and whether a
    # pass starts on content another surface overwrote since this surface last wrote it.
    HOST_CLASS = {0: 'rgba8', 1: 'rgba8', 2: 'rgb10a2', 10: 'rgb10a2', 3: 'rgba16f', 12: 'rgba16f',
                  7: 'rgba16f', 4: 'rg16', 5: 'rgba16', 6: 'rg16f', 14: 'r32f', 15: 'rg32f'}
    writes = []  # (start, end, key, seq)
    last_write = {}  # key -> seq of its last write or clear
    seq = 0
    pending_clear = None  # Clear/ClearF applies to the surfaces of the next pass

    def host_key(surf, info, depth):
        return ('D' if depth else 'C', info & 0xFFF, surf & 0x3FFF, (surf >> 16) & 3,
                (info >> 16) & 1 if depth else HOST_CLASS.get((info >> 16) & 0xF, '?'))

    def status(key, start, end):
        mine = last_write.get(key)
        if mine is None:
            return 'NEW'
        for s, e, k, q in writes:
            if q > mine and k != key and s < end and start < e:
                return f'ALIASED(by {k[0]} base={k[1]} pitch={k[2]} msaa={k[3]} {k[4]})'
        return 'kept'

    def touch(key, start, end):
        nonlocal seq
        seq += 1
        writes.append((start, end, key, seq))
        last_write[key] = seq

    rows = []
    print(f'# {a.trace}')
    for ev in passes:
        if ev[0] == 'pass':
            (surf, c0, dinfo, mrt), p = ev[1], ev[2]
            w, h = p['scmax']
            uses = ('color ' if p['color'] else '') + ('zr ' if p['zread'] else '') + ('zw ' if p['zwrite'] else '') + ('st' if p['stencil'] else '')
            line = f'PASS {p["draws"]:5d} draws  {surf_desc(surf, c0, False)} | {surf_desc(surf, dinfo, True)}  sc<={w}x{h}  [{uses.strip()}]'
            if p['mrt']:
                line += '  MRT ' + ' '.join(f'c{i}={surf_desc(surf, v, False)}' for i, v in sorted(p['mrt']))
            rows_h = min(h, 2048)
            notes = []
            for depth, info, used in ((False, c0, p['color']), (True, dinfo, p['zread'] or p['zwrite'] or p['stencil'])):
                if not used:
                    continue
                k = host_key(surf, info, depth)
                start = info & 0xFFF
                end = start + tiles(surf, info, depth, rows_h)
                if pending_clear:
                    notes.append(f'{k[0]}:cleared')
                else:
                    notes.append(f'{k[0]}:{status(k, start, end)}')
                touch(k, start, end)
            pending_clear = None
            line += f'  tiles C={tiles(surf, c0, False, rows_h)} D={tiles(surf, dinfo, True, rows_h)}  ' + ' '.join(notes)
            print(line)
        elif ev[0] == 'clear':
            c = ev[1]
            pending_clear = c
            if c['fn'] == 'Clear':
                # The stencil is in r9 (Z in f1 takes r8's slot), which C lines do not record.
                print(f'CLEAR c={c["seq"]} count={int(c["r4"], 16)} rects={c["r5"]} flags={c["r6"]} color={c["r7"]}')
            else:
                print(f'CLEARF c={c["seq"]} flags={c["r4"]} rect={c["r5"]} color_ptr={c["r6"]} stencil={c["r8"]}')
        else:
            d, c = ev[1], ev[2]
            ctl, dest, pitch, info = (int(d[k], 16) for k in ('copy_ctl', 'dest', 'pitch', 'info'))
            surf, c0, dinfo = (int(d[k], 16) for k in ('surf', 'c0', 'd'))
            src = ctl & 7
            depth = src == 4
            fmt = (info >> 7) & 0x3F
            flags = int(c['r4'], 16) if c else 0
            if flags & 0x300:  # Resolve's clear: the source color surface and the depth surface
                if flags & 0x100:
                    k = host_key(surf, c0, False)
                    touch(k, c0 & 0xFFF, (c0 & 0xFFF) + 1)
                if flags & 0x200:
                    k = host_key(surf, dinfo, True)
                    touch(k, dinfo & 0xFFF, (dinfo & 0xFFF) + 1)
            print(f'RESOLVE c={d["c"]} flags={flags:X} srcRect={c["r5"] if c else "-"} destTex={c["r6"] if c else "-"} '
                  f'level={int(c["r8"], 16) if c else "-"} from {surf_desc(surf, dinfo if depth else c0, depth)} '
                  f'-> {dest:08X} {TEX_FMT.get(fmt, fmt)} {pitch & 0x3FFF}x{(pitch >> 16) & 0x3FFF} '
                  f'cmd={(ctl >> 20) & 3} sample={(ctl >> 4) & 7} clearC={(ctl >> 8) & 1} clearD={(ctl >> 9) & 1} '
                  f'endian={info & 7} array={(info >> 3) & 1} slice={(info >> 4) & 7} number={(info >> 13) & 7} '
                  f'bias={(info >> 16) & 0x3F} swap={(info >> 24) & 1}')
            rows.append([d['c'], f'{flags:X}', c['r5'] if c else '', c['r6'] if c else '', c['r8'] if c else '',
                         'depth' if depth else f'color{src}', f'{surf:08X}', f'{(dinfo if depth else c0):08X}',
                         f'{dest:08X}', TEX_FMT.get(fmt, str(fmt)), str(pitch & 0x3FFF), str((pitch >> 16) & 0x3FFF),
                         f'{ctl:08X}', f'{info:08X}'])
    if a.resolves_csv:
        with open(a.resolves_csv, 'w') as f:
            f.write('call,flags,src_rect,dest_tex,level,source,surface_info,source_info,dest_base,dest_format,'
                    'dest_pitch,dest_height,copy_control,dest_info\n')
            for r in rows:
                f.write(','.join(r) + '\n')


if __name__ == '__main__':
    main()
