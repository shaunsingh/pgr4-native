#!/usr/bin/env python3
"""Map every hardcoded PGR4 render-resolution constant to its guest address.

Uses tu_address_map's listing of the generated code (function starts and loc_ labels give each
`//` instruction comment its address) and prints every instruction mentioning a resolution-like
constant, so games/pgr4's manifest can hook them.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tu_address_map import listings

GEN = os.path.expanduser('~/recomp/builds/pgr4/native-deck/game/pgr4/generated/default')
# Resolution-like operands: the dimensions and their halves as immediates, and the float
# constants 1280.0 (0x44A00000), 720.0 (0x44340000), 640.0 (0x44200000), 360.0 (0x43B40000).
IMM = re.compile(r'(?<![\w.])(1280|720|640|360)(?![\w.(])')
FLT = re.compile(r'(?<![\w.])(0x44A00000|0x44340000|0x44200000|0x43B40000)(?![\w.])', re.I)

for func in sorted(listings(GEN).items()):
    for addr, insn in func[1]:
        if IMM.search(insn) or FLT.search(insn):
            print(f'0x{addr:08X}  sub_{func[0]:08X}  {insn}')
