#!/usr/bin/env python3
"""Find code that references an address in the game: absolute imm32/disp32 operands and rip-relative operands.

  python tools/xref.py <addr> [<addr_hi>]     every reference to [addr, addr_hi] (default: addr only)

Candidates are found by byte search (abs) and by decoding every function that contains a rip-relative operand
pointing into the range (rip), using the FDE function list (tools/funcs.py). Output: address, function start,
instruction.
"""
import argparse, bisect, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf
from funcs import fde_ranges
from disasm import default_game
import capstone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("hi", nargs="?")
    ap.add_argument("--elf")
    a = ap.parse_args()
    lo = int(a.addr, 16)
    hi = int(a.hi, 16) if a.hi else lo
    e = Elf(a.elf or default_game())
    funcs = fde_ranges(e)
    starts = [f[0] for f in funcs]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    seen = set()
    hits = []
    seg = [p for p in e.phdrs if p["type"] == 1][0]
    code = e.d[seg["off"]:seg["off"] + seg["filesz"]]
    # candidate functions: any containing the 4 bytes of an absolute address in range, or a disp32 that rip-relative
    # resolves into range (checked by decoding).
    cand = set()
    for v in range(lo & ~3, hi + 1, 1) if hi - lo < 4096 else []:
        for m in re.finditer(re.escape(struct.pack("<I", v)), code):
            cand.add(seg["vaddr"] + m.start())
    fset = set()
    for c in cand:
        i = bisect.bisect_right(starts, c) - 1
        if i >= 0 and funcs[i][0] <= c < funcs[i][1]:
            fset.add(i)
    # rip-relative: brute-force decode functions whose disp could reach (all functions; slower but complete)
    if hi - lo < 0x100000:
        for i, (fa, fb) in enumerate(funcs):
            o = fa - seg["vaddr"]
            body = code[o:o + (fb - fa)]
            # quick filter: rip disp = target - next_ip; try every 4-byte window
            for k in range(0, len(body) - 4):
                d = struct.unpack_from("<i", body, k)[0]
                t = fa + k + 4 + d
                if lo <= t <= hi + 8:  # next_ip is at most a few bytes beyond the disp
                    fset.add(i)
                    break
    for i in sorted(fset):
        fa, fb = funcs[i]
        o = fa - seg["vaddr"]
        for ins in md.disasm(code[o:o + (fb - fa)], fa):
            hit = False
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_IMM and lo <= op.imm <= hi:
                    hit = True
                if op.type == capstone.x86.X86_OP_MEM:
                    if op.mem.base == capstone.x86.X86_REG_RIP and lo <= ins.address + ins.size + op.mem.disp <= hi:
                        hit = True
                    elif op.mem.base == 0 and lo <= op.mem.disp <= hi:
                        hit = True
            if hit and ins.address not in seen:
                seen.add(ins.address)
                print(f"{ins.address:#010x}  (func {fa:#x})  {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
