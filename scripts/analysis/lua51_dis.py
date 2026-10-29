#!/usr/bin/env python3
"""Disassemble the game's Lua 5.1 UI scripts (the bytecode PGR4's executable embeds).

The executable carries its UI scripts as a "BZUI" archive in its data: a big-endian count, then
per script {name offset, data offset, compressed size, size} (offsets from the name table, which
follows the entries), each a zlib stream of Lua 5.1 bytecode compiled little-endian with 4-byte
ints and size_t. --extract IMAGE DIR writes them out as DIR/<name>c; a .luac argument prints its
listing: every function with its constants, upvalue names and locals, and each instruction with
its RK operands and jump targets resolved, the way `luac -l -l` would.

  lua51_dis.py --extract <image.bin> <dir>
  lua51_dis.py <script.luac> [...]
"""

import os
import struct
import sys
import zlib

OPS = ("MOVE LOADK LOADBOOL LOADNIL GETUPVAL GETGLOBAL GETTABLE SETGLOBAL SETUPVAL SETTABLE "
       "NEWTABLE SELF ADD SUB MUL DIV MOD POW UNM NOT LEN CONCAT JMP EQ LT LE TEST TESTSET CALL "
       "TAILCALL RETURN FORLOOP FORPREP TFORLOOP SETLIST CLOSE CLOSURE VARARG").split()


def extract(image, out):
    d = open(image, "rb").read()
    at = d.find(b"BZUI")
    if at < 0:
        sys.exit(f"{image}: no BZUI archive")
    (count,) = struct.unpack_from(">I", d, at + 4)
    entries = at + 8
    names = entries + 16 * count
    for i in range(count):
        name_off, data_off, packed, size = struct.unpack_from(">4I", d, entries + 16 * i)
        name = d[names + name_off:d.index(b"\0", names + name_off)].decode()
        data = zlib.decompress(d[names + data_off:names + data_off + packed])
        if len(data) != size:
            sys.exit(f"{name}: {len(data)} bytes, the directory says {size}")
        path = os.path.join(out, name.replace("\\", "/") + "c")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    print(f"{count} scripts -> {out}")


class Reader:
    def __init__(self, data):
        if data[:12] != b"\x1bLuaQ\x00\x01\x04\x04\x04\x08\x00":
            sys.exit("not little-endian Lua 5.1 bytecode with 4-byte ints and size_t")
        self.d, self.p = data, 12

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.take("I")
        s = self.d[self.p:self.p + n - 1].decode("latin-1") if n else None
        self.p += n
        return s

    def function(self):
        f = {"source": self.string()}
        f["line"], f["last"], f["nups"], f["params"], f["vararg"], f["stack"] = self.take("iiBBBB")
        f["code"] = [self.take("I") for _ in range(self.take("i"))]
        consts = []
        for _ in range(self.take("i")):
            t = self.take("B")
            consts.append(None if t == 0 else bool(self.take("B")) if t == 1 else
                          self.take("d") if t == 3 else self.string())
        f["consts"] = consts
        f["protos"] = [self.function() for _ in range(self.take("i"))]
        f["lines"] = [self.take("i") for _ in range(self.take("i"))]
        f["locals"] = [(self.string(), *self.take("ii")) for _ in range(self.take("i"))]
        f["upvals"] = [self.string() for _ in range(self.take("i"))]
        return f


def const(v):
    if isinstance(v, str):
        return '"' + v.encode("unicode_escape").decode() + '"'
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return repr(v)


def dump(f, path="main"):
    consts = f["consts"]
    rk = lambda x: const(consts[x - 256]) if x >= 256 else f"r{x}"
    print(f"\nfunction {path} (lines {f['line']}-{f['last']}, {f['params']} params"
          f"{', vararg' if f['vararg'] else ''}, {f['nups']} upvalues, {len(f['code'])} insns)")
    if f["upvals"]:
        print("  upvalues: " + ", ".join(f["upvals"]))
    if f["locals"]:
        print("  locals: " + ", ".join(f"{n}@{a}-{b}" for n, a, b in f["locals"]))
    for pc, i in enumerate(f["code"]):
        op, a = OPS[i & 0x3F], (i >> 6) & 0xFF
        c, b, bx = (i >> 14) & 0x1FF, (i >> 23) & 0x1FF, i >> 14
        sbx = bx - 131071
        line = f["lines"][pc] if pc < len(f["lines"]) else ""
        if op in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
            arg = f"r{a} {const(consts[bx])}"
        elif op in ("JMP", "FORLOOP", "FORPREP"):
            arg = f"r{a} -> {pc + 1 + sbx}"
        elif op == "CLOSURE":
            arg = f"r{a} {path}.{bx}"
        elif op in ("GETTABLE", "SELF"):
            arg = f"r{a} r{b}[{rk(c)}]"
        elif op == "SETTABLE":
            arg = f"r{a}[{rk(b)}] = {rk(c)}"
        elif op in ("ADD", "SUB", "MUL", "DIV", "MOD", "POW", "EQ", "LT", "LE"):
            arg = f"r{a} {rk(b)} {rk(c)}"
        elif op in ("GETUPVAL", "SETUPVAL"):
            arg = f"r{a} {f['upvals'][b] if b < len(f['upvals']) else 'u%d' % b}"
        else:
            arg = f"{a} {b} {c}"
        print(f"  {pc:4} [{line}] {op:9} {arg}")
    for n, p in enumerate(f["protos"]):
        dump(p, f"{path}.{n}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--extract":
        extract(sys.argv[2], sys.argv[3])
    elif len(sys.argv) > 1:
        for path in sys.argv[1:]:
            print(f"-- {path}")
            dump(Reader(open(path, "rb").read()).function())
    else:
        sys.exit(__doc__)
