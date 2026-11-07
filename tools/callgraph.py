#!/usr/bin/env python3
"""Direct-call graph of the game, cached in tmp/callgraph.json (keyed on the game's size+mtime).

  python tools/callgraph.py callers <addr>          functions that call addr
  python tools/callgraph.py callees <addr>          functions addr calls
  python tools/callgraph.py boundary <lo> <hi>      functions in [lo,hi) called from outside it, with caller counts
  python tools/callgraph.py strings <addr>          strings a function references (imm32 / rip-relative)

Functions are FDE ranges (tools/funcs.py).
"""
import bisect, json, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf
from funcs import fde_ranges
from disasm import default_game
import capstone

CACHE = os.path.join(os.path.dirname(__file__), "..", "tmp", "callgraph.json")


def build(path):
    st = os.stat(path)
    key = f"{st.st_size}:{int(st.st_mtime)}"
    if os.path.exists(CACHE):
        c = json.load(open(CACHE))
        if c.get("key") == key:
            return c
    e = Elf(path)
    funcs = fde_ranges(e)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    seg = [p for p in e.phdrs if p["type"] == 1][0]
    code = e.d[seg["off"]:seg["off"] + seg["filesz"]]
    calls = {}
    refs = {}
    for fa, fb in funcs:
        o = fa - seg["vaddr"]
        out, rr = set(), set()
        for ins in md.disasm(code[o:o + (fb - fa)], fa):
            if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 16)
                if not (fa <= t < fb):
                    out.add(t)
            for m in re.finditer(r"0x([0-9a-f]{6,8})\b", ins.op_str):
                rr.add(int(m.group(1), 16))
            m = re.search(r"\[rip \+ 0x([0-9a-f]+)\]", ins.op_str)
            if m:
                rr.add(ins.address + ins.size + int(m.group(1), 16))
        calls[str(fa)] = sorted(out)
        refs[str(fa)] = sorted(rr)
    c = {"key": key, "funcs": funcs, "calls": calls, "refs": refs}
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(c, open(CACHE, "w"))
    return c


def main():
    path = default_game()
    c = build(path)
    e = Elf(path)
    funcs = c["funcs"]
    starts = [f[0] for f in funcs]
    func_of = lambda a: starts[bisect.bisect_right(starts, a) - 1]
    cmd = sys.argv[1]
    if cmd == "callers":
        t = int(sys.argv[2], 16)
        for f, outs in c["calls"].items():
            if t in outs:
                print(hex(int(f)))
    elif cmd == "callees":
        for t in c["calls"][str(int(sys.argv[2], 16))]:
            print(hex(t))
    elif cmd == "boundary":
        lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
        cnt = {}
        for f, outs in c["calls"].items():
            if lo <= int(f) < hi:
                continue
            for t in outs:
                if lo <= t < hi:
                    cnt.setdefault(t, set()).add(int(f))
        for t in sorted(cnt):
            strs = []
            for r in c["refs"].get(str(t), []):
                try:
                    s = e.cstr(r)
                    if 3 <= len(s) < 70 and s.isprintable():
                        strs.append(s)
                except (KeyError, ValueError):
                    pass
            print(f"{t:#x}  callers {len(cnt[t]):3}  {' | '.join(strs[:3])}")
    elif cmd == "strings":
        for r in c["refs"].get(str(int(sys.argv[2], 16)), []):
            try:
                s = e.cstr(r)
                if 2 <= len(s) < 120 and s.isprintable():
                    print(hex(r), s)
            except (KeyError, ValueError):
                pass


if __name__ == "__main__":
    main()
