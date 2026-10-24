#!/usr/bin/env python3
"""Guest addresses across two builds of the executable: the base disc's (0.0.0.1) and Title
Update #2's (0.0.2.1), from the C++ ReXGlue generated for each.

The title update relinks the whole executable, so nearly every function moves; most are
unchanged. Each instruction's disassembly is in the generated code as a comment (`// lwz
r31,12688(r30)`), its address follows from the function's start, the `loc_` labels and the
return addresses after calls. Functions are matched on that listing: first those whose listing
(branch targets made relative, data addresses built with lis masked) is unique in both
builds, then those unique with every number masked, keeping only matches in address order;
the functions between two matches pair up by position when both gaps hold the same number. An
address inside a function maps to the same instruction of its counterpart, through a diff of
the two listings when the function changed.

  tu_address_map.py map BASE_GEN TU_GEN ADDR...     one line per address: base, TU, how
  tu_address_map.py retarget MAP FILE...
                                                   rewrites the base sub_/loc_/0x addresses in
                                                   FILEs with MAP's (retired/title-update/addresses.txt)

BASE_GEN and TU_GEN are generated/default directories (codegen of each executable with the
same manifest options; game/pgr4/pgr4recompiled_manifest.toml for the TU's). Their parsed listings are
cached beside this script's output in $TMPDIR. `retarget` fails on an address MAP lacks.
"""
import argparse, bisect, difflib, glob, hashlib, os, pickle, re, sys, tempfile
from collections import Counter

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
INSN = re.compile(r'^\t// ([a-z][a-z0-9.+\-]*(?: [^\n]*)?)$')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')
NUM = re.compile(r'-?(?:0x[0-9a-fA-F]+|\d+)')
ADDR = re.compile(r'0x8[0-9a-f]{7}')
DFORM = re.compile(r'^(-?\d+)\((r\d+)\)$')
# No word boundary in front: PowerShell's `n and `t run into the label (`nloc_822A5520).
TOKEN = re.compile(r'(0x|sub_|loc_)(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')


def listings(tree):
    """{function: [(address, instruction)]} of a generated tree, cached by tree contents."""
    files = sorted(glob.glob(os.path.join(tree, '*_recomp.*.cpp')))
    if not files:
        sys.exit(f'no generated code in {tree}')
    key = hashlib.sha1(repr([(f, os.path.getmtime(f), os.path.getsize(f)) for f in files]).encode()).hexdigest()[:16]
    cache = os.path.join(tempfile.gettempdir(), f'tu_address_map-{key}.pkl')
    if os.path.exists(cache):
        with open(cache, 'rb') as f:
            return pickle.load(f)
    funcs = {}
    for path in files:
        text = open(path, encoding='utf-8', errors='replace').read()
        starts = [(m.start(), int(m.group(1), 16)) for m in FUNC.finditer(text)]
        for i, (pos, addr) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            insns, cur = [], addr
            for line in text[pos:end].split('\n'):
                m = INSN.match(line)
                if m:
                    insns.append((cur, m.group(1)))
                    cur += 4
                elif (m := LABEL.match(line)):
                    cur = int(m.group(1), 16)   # jump tables leave gaps in the listing
            funcs[addr] = insns
    with open(cache, 'wb') as f:
        pickle.dump(funcs, f)
    return funcs


def loose(insn):
    return NUM.sub('#', insn)


def strict(insns, lo, hi):
    out, lis = [], set()
    for addr, text in insns:
        mn, _, ops = text.partition(' ')
        parts = [ADDR.sub(lambda m: f'@{int(m.group(0), 16) - addr:+d}'
                          if lo <= int(m.group(0), 16) < hi else '@', p)
                 for p in (ops.split(',') if ops else [])]
        if mn == 'lis' and len(parts) == 2:
            lis.add(parts[0])
            parts[1] = '#'
        elif parts:
            based = False
            if mn in ('addi', 'addic', 'ori', 'oris', 'addis') and len(parts) == 3 and parts[1] in lis:
                parts[2], based = '#', True
            else:
                for k, p in enumerate(parts):
                    if (m := DFORM.match(p)) and m.group(2) in lis:
                        parts[k], based = f'#({m.group(2)})', True
            if mn in ('addi', 'ori', 'addis') and based:
                lis.add(parts[0])
            elif not mn.startswith(('st', 'cmp', 'b')):
                lis.discard(parts[0])
        out.append(mn + ' ' + ','.join(parts))
    return hash(tuple(out))


def keys(funcs):
    starts = sorted(funcs)
    return {a: (strict(funcs[a], a, starts[i + 1] if i + 1 < len(starts) else 1 << 32),
                hash(tuple(loose(t) for _, t in funcs[a])))
            for i, a in enumerate(starts)}


def match(base, tu):
    bk, tk = keys(base), keys(tu)
    pairs, kind = {}, {}
    for level, name in ((0, 'same'), (1, 'same-shape')):
        used = set(pairs.values())
        bc = Counter(k[level] for a, k in bk.items() if a not in pairs)
        tc = Counter(k[level] for a, k in tk.items() if a not in used)
        unique = {k[level]: a for a, k in tk.items() if a not in used and tc[k[level]] == 1}
        for a, k in bk.items():
            if a not in pairs and bc[k[level]] == 1 and (t := unique.get(k[level])) is not None:
                pairs[a], kind[a] = t, name
    # Keep the longest run of matches in address order.
    anchors = sorted(pairs.items())
    tails, tail_idx, prev = [], [], [-1] * len(anchors)
    for i, (_, t) in enumerate(anchors):
        j = bisect.bisect_left(tails, t)
        if j == len(tails):
            tails.append(t), tail_idx.append(i)
        else:
            tails[j], tail_idx[j] = t, i
        prev[i] = tail_idx[j - 1] if j else -1
    keep, k = set(), tail_idx[-1] if tail_idx else -1
    while k >= 0:
        keep.add(anchors[k][0])
        k = prev[k]
    for a, _ in anchors:
        if a not in keep:
            del pairs[a], kind[a]
    bs, ts = sorted(base), sorted(tu)
    bpos, tpos = {a: i for i, a in enumerate(bs)}, {a: i for i, a in enumerate(ts)}
    anchors = sorted(pairs.items())
    for (a0, t0), (a1, t1) in zip(anchors, anchors[1:]):
        bg, tg = bs[bpos[a0] + 1:bpos[a1]], ts[tpos[t0] + 1:tpos[t1]]
        if bg and len(bg) == len(tg):
            for a, t in zip(bg, tg):
                pairs[a] = t
                kind[a] = 'by-position' if bk[a][1] == tk[t][1] else 'by-position, changed'
    return pairs, kind


def map_address(addr, base, tu, pairs, kind, starts):
    if addr in base:
        t = pairs.get(addr)
        return t, f'function, {kind.get(addr, "unmatched")}'
    i = bisect.bisect_right(starts, addr) - 1
    func = starts[i] if i >= 0 else None
    if func is None or not base[func] or base[func][-1][0] < addr:
        return None, 'not code'
    where = f'sub_{func:08X}+{addr - func:#x}'
    if (t := pairs.get(func)) is None:
        return None, f'{where}, function unmatched'
    bl, tl = [loose(x) for _, x in base[func]], [loose(x) for _, x in tu[t]]
    idx = next((k for k, (a, _) in enumerate(base[func]) if a == addr), None)
    if idx is None:
        return None, f'{where}, not an instruction'
    for tag, i1, i2, j1, _ in difflib.SequenceMatcher(None, bl, tl, autojunk=False).get_opcodes():
        if i1 <= idx < i2:
            if tag != 'equal':
                return None, f'{where}, instruction changed ({tag})'
            return tu[t][j1 + idx - i1][0], f'{where}, {kind[func]}'
    return None, where


def read_map(path):
    table = {}
    for line in open(path, encoding='utf-8'):
        fields = line.split('#', 1)[0].split()
        if len(fields) >= 2 and re.fullmatch(r'82[0-9A-Fa-f]{6}', fields[0]):
            table[int(fields[0], 16)] = None if fields[1] == '-' else int(fields[1], 16)
    return table


def retarget_text(text, table, path):
    # Addresses already rewritten stay, so a second run changes nothing.
    done = {t for t in table.values() if t is not None}
    if overlap := done & table.keys():
        sys.exit(f'map names {", ".join(f"{a:08X}" for a in sorted(overlap))} on both sides')
    missing = set()
    def sub(m):
        a = int(m.group(2), 16)
        if a in done:
            return m.group(0)
        t = table.get(a)
        if t is None:
            missing.add(m.group(0))
            return m.group(0)
        new = f'{t:08X}'
        return m.group(1) + (new.lower() if m.group(2) != m.group(2).upper() else new)
    out = TOKEN.sub(sub, text)
    if missing:
        sys.exit(f'{path}: no title update address for {", ".join(sorted(missing))}; '
                 'find it with tu_address_map.py map and add it to the map')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('map')
    m.add_argument('base_gen'), m.add_argument('tu_gen'), m.add_argument('addr', nargs='+')
    r = sub.add_parser('retarget')
    r.add_argument('map'), r.add_argument('files', nargs='+')
    args = ap.parse_args()
    if args.cmd == 'map':
        base, tu = listings(args.base_gen), listings(args.tu_gen)
        pairs, kind = match(base, tu)
        c = Counter(kind.values())
        print(f'# {len(pairs)} of {len(base)} functions matched ({", ".join(f"{v} {k}" for k, v in c.items())})')
        starts = sorted(base)
        for a in args.addr:
            addr = int(a.removeprefix('0x').removeprefix('sub_').removeprefix('loc_'), 16)
            t, how = map_address(addr, base, tu, pairs, kind, starts)
            print(f'{addr:08X} {t:08X}  {how}' if t else f'{addr:08X} -         {how}')
    else:
        table = read_map(args.map)
        for path in args.files:
            text = open(path, encoding='utf-8', newline='').read()
            new = retarget_text(text, table, path)
            if new != text:
                open(path, 'w', encoding='utf-8', newline='').write(new)


if __name__ == '__main__':
    main()
