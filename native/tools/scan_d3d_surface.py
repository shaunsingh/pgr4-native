#!/usr/bin/env python3
"""Lists the guest D3D API surface of PGR4 (Title Update #2's default.xex) from ReXGlue generated C++.

Run on a copy of the generated code (it stays off git), e.g.:

  scan_d3d_surface.py --generated generated/default \
      --registers <sdk-install>/include/rex/graphics/register_table.inc > ../src/d3d_surface.inc

Method:
- The XDK D3D library sits in LIB_LO..LIB_HI (all PM4 packet builders found by DrawCounter are
  inside it; the device code uses the D3D device layout below).
- An API entry point is a function in that range with at least one direct caller outside it
  (game code). Internal draw builders are added explicitly because DrawCounter counted them.
- The D3D device keeps a shadow copy of the GPU registers. The draw functions flush dirty
  blocks with type-0 packets copied verbatim from these device offsets (see the
  `addi r6,r31,OFF ; li r5,REG ; bl 0x826ddc90/0x826de030` sequences in sub_826D0340). A small
  setter that stores into one of these offsets and sets a dirty bit in the 64-bit masks at
  device+0..40 is named after the register it changes; a small getter after the register it
  reads.
- The remaining names were assigned by reading the functions (evidence in
  the development notes, "Renderer decision").
"""
import argparse
import collections
import glob
import os
import re
import sys

LIB_LO, LIB_HI = 0x826C33C0, 0x826E17F0

# device offset -> (first register, register count); from the flush calls in sub_826D0340.
SHADOW = [
    (1152, 0x4800, 192),   # fetch constants (32 x 6 dwords)
    (1920, 0x4000, 1024),  # vertex shader float constants
    (6016, 0x4400, 1024),  # pixel shader float constants
    (10112, 0x4900, 40),   # bool + loop constants
    (10368, 0x2000, 19),
    (10444, 0x2100, 21),
    (10528, 0x2180, 5),
    (10548, 0x2200, 12),
    (10596, 0x2280, 21),
    (10680, 0x2300, 38),
    (10832, 0x2380, 14),
]

# Functions named by reading them. (address, name, category)
KNOWN = {
    0x826D0340: ("DrawIndexedVertices", "kDraw"),       # DRAW_INDX with index buffer (dev+12428)
    0x826CFF58: ("DrawVertices", "kDraw"),              # DRAW_INDX, auto index
    0x826CFA60: ("BeginVertices", "kDraw"),             # DRAW_INDX auto index, returns inline space
    0x826D0798: ("DrawIndx2Inline", "kDraw"),           # internal: DRAW_INDX_2 with inline indices
    0x826DC888: ("DrawIndx2Split", "kDraw"),            # internal: DRAW_INDX_2 split at 65534
    0x826CFF10: ("DrawVerticesUP", "kDraw"),            # BeginVertices + memcpy + restore push
    0x826CBEE0: ("Resolve", "kResolve"),                # 3-vertex rect DRAW_INDX_2 in copy mode
    0x826CB148: ("Swap", "kSwap"),                      # calls VdSwap / VdGetSystemCommandBuffer
    0x826CB140: ("KickOff", "kPush"),                   # li r4,0 ; b 0x826CACA0
    0x826CACA0: ("KickOffEx", "kPush"),
    0x826CE8C0: ("MakeSpace", "kPush"),                 # push pointer dev+48 past limit dev+56
    0x826CEB48: ("InsertFence", "kPush"),
    0x826CFF00: ("RestorePush", "kPush"),               # dev+48 = dev+13428
    0x826CE738: ("SwitchSegment", "kPush"),             # internal: dev+48 = new segment - 4
    0x826CE4E0: ("StartSegment", "kPush"),              # internal: dev+48 = dev+16712 / new - 4
    0x826CD868: ("RestoreSegment", "kPush"),            # internal: dev+48 = dev+16712
    0x826CD380: ("RestoreSegment13392", "kPush"),       # internal: dev+48 = dev+13392
    0x826CE340: ("BlockOnResource", "kSync"),
    0x826CE320: ("BlockOnFence", "kSync"),
    0x826C9908: ("SetVertexShaderConstantF", "kConst"),  # dev+1920 + 16*reg
    0x826C99F0: ("SetPixelShaderConstantF", "kConst"),   # dev+6016 + 16*reg
    0x826C9AD8: ("GetVertexShaderConstantF", "kConst"),
    0x826C9AF8: ("GetPixelShaderConstantF", "kConst"),
    0x826C9B18: ("SetVertexShaderConstantB", "kConst"),  # dev+10112 bit array
    0x826C9B78: ("SetPixelShaderConstantB", "kConst"),   # dev+10128 bit array
    0x826C9E30: ("SetVertexShader", "kShader"),          # dev+12684
    0x826C9FF0: ("GetVertexShader", "kShader"),
    0x826CA130: ("SetPixelShader", "kShader"),           # dev+12688
    0x826CA300: ("GetPixelShader", "kShader"),
    0x826CA348: ("SetVertexDeclaration", "kShader"),     # dev+11812
    0x826CA360: ("GetVertexDeclaration", "kShader"),
    0x826C8D60: ("SetTexture", "kTexture"),              # fetch slot dev+1152+24*stage, ptr dev+12536+4*stage
    0x826C8EE0: ("GetTexture", "kTexture"),
    0x826C4FE8: ("SetSamplerState_A", "kTexture"),       # fetch slot dwords 3-4
    0x826C5178: ("SetSamplerState_B", "kTexture"),
    0x826C5B28: ("SetStreamSource", "kStream"),          # vertex fetch constant 95-stream
    0x826C5C48: ("GetStreamSource", "kStream"),
    0x826C5CD0: ("SetIndices", "kStream"),               # dev+12428
    0x826C5D60: ("GetIndices", "kStream"),
    0x826C64B0: ("SetRenderTarget", "kSurface"),         # dev+12432+4*index
    0x826C6AF0: ("SetRenderTargetThunk", "kSurface"),    # b 0x826c64b0
    0x826C5DA0: ("GetRenderTarget", "kSurface"),
    0x826C6818: ("SetDepthStencilSurface", "kSurface"),  # dev+12448
    0x826C5DE8: ("GetDepthStencilSurface", "kSurface"),
    0x826C6378: ("SetViewport", "kState"),
    0x826D1A08: ("Clear", "kClear"),
    0x826D18E0: ("ClearF", "kClear"),
    0x826D62C8: ("SetBinMask", "kTiling"),               # SET_BIN_MASK_LO/HI (DrawCounter)
    0x826D2F00: ("QueryIssue", "kQuery"),                # query types 9 (occlusion) / 10
    0x826D3188: ("QueryGetData", "kQuery"),
    0x826CD270: ("CreateDevice", "kDevice"),             # allocates the 22,400-byte device
    0x826CF670: ("ResourceRelease", "kResource"),        # atomic refcount at +4
}

# Internal functions hooked as well: the two draw builders DrawCounter counted, MakeSpace, and
# the functions that assign the push pointer (dev+48) to a new segment; the trace tap decodes up
# to the old pointer at their entry and skips the hole they leave.
INTERNAL_EXTRA = [0x826D0798, 0x826DC888, 0x826CE8C0, 0x826CE738, 0x826CE4E0, 0x826CD868, 0x826CD380]


def load_registers(path):
    regs = {}
    for m in re.finditer(r'XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)', open(path).read()):
        regs.setdefault(int(m.group(1), 16), m.group(2))
    return regs


def shadow_reg(off):
    for base, reg, n in SHADOW:
        if base <= off < base + 4 * n:
            return reg + (off - base) // 4
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--generated', required=True)
    ap.add_argument('--registers', required=True)
    a = ap.parse_args()
    regs = load_registers(a.registers)

    funcs = {}
    for f in sorted(glob.glob(os.path.join(a.generated, '*_recomp.*.cpp'))):
        src = open(f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{\n(.*?)(?=^DEFINE_REX_FUNC|\Z)',
                             src, re.S | re.M):
            funcs[int(m.group(1), 16)] = m.group(2)
    if not funcs:
        sys.exit('no DEFINE_REX_FUNC bodies found')
    callers = collections.defaultdict(set)
    for addr, body in funcs.items():
        for c in set(re.findall(r'\bsub_([0-9A-F]{8})\(ctx, base\)', body)):
            callers[int(c, 16)].add(addr)

    def inlib(x):
        return LIB_LO <= x < LIB_HI

    entries = sorted(a_ for a_ in funcs if inlib(a_) and any(not inlib(c) for c in callers[a_]))
    for x in INTERNAL_EXTRA:
        if x not in entries:
            entries.append(x)
    entries.sort()

    used = collections.Counter()
    out = []
    for addr in entries:
        body = funcs[addr]
        asm = [l.strip()[3:] for l in body.splitlines() if l.strip().startswith('// ')]
        ext = sum(1 for c in callers[addr] if not inlib(c))
        if addr in KNOWN:
            name, cat = KNOWN[addr]
        else:
            stores = [int(o) for o in re.findall(r'^st(?:[wbh]|fs) [rf]\d+,(\d+)\(r3\)$', '\n'.join(asm), re.M)]
            loads = [int(o) for o in re.findall(r'^l[wbh]z r\d+,(\d+)\(r3\)$', '\n'.join(asm), re.M)]
            dirty = any(re.match(r'^std r\d+,(0|8|16|24|32|40)\(r3\)$', l) for l in asm)
            sregs = sorted({shadow_reg(o & ~3) for o in stores if shadow_reg(o & ~3) is not None})
            name, cat = None, 'kOther'
            if len(asm) <= 40 and dirty and sregs:
                name = 'SetRS_' + '_'.join(regs.get(r, f'{r:04X}') for r in sregs[:2])
                cat = 'kState'
            elif len(asm) <= 4 and not stores and loads and asm[-1] == 'blr':
                lregs = [shadow_reg(o & ~3) for o in loads]
                if lregs[0] is not None:
                    name = 'GetRS_' + regs.get(lregs[0], f'{lregs[0]:04X}')
                else:
                    name = f'GetField_{loads[0]}'
                cat = 'kGet'
            elif len(asm) <= 40 and stores and not any(l.startswith('bl ') for l in asm):
                name = f'SetField_{stores[0]}'
                cat = 'kState'
            if name is None:
                name = f'fn_{addr:08X}'
        used[name] += 1
        if used[name] > 1:
            name = f'{name}_{addr:08X}'
        out.append((addr, name, cat, ext, len(asm)))

    print('// Generated by native/tools/scan_d3d_surface.py from the ReXGlue')
    print('// generated C++ of Title Update #2\'s default.xex (0.0.2.1: the disc\'s f9a411d8...f423e with')
    print('// title-update/default.xexp). Do not edit by hand.')
    print('// D3D_FN(address, name, category)  // external call sites, instructions')
    for addr, name, cat, ext, n in out:
        print(f'D3D_FN({addr:08X}, "{name}", {cat})  // ext={ext} n={n}')
    print(f'// {len(out)} functions', file=sys.stderr)


if __name__ == '__main__':
    main()
