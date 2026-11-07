#!/usr/bin/env python3
"""Find every FS-segment instruction in a Linux ELF's code (Linux TLS and the stack protector use FS; Windows can't
keep an FS base for us, see docs/architecture.md#thread-local-storage).

Disassembles every FDE range (tools/funcs.py) with capstone and classifies each fs: access:
  abs   fs:[disp32]            (stack canary fs:0x28, TCB self fs:0, ...) -> rewritten in place to gs:[TEB TLS slot]
  reg   fs:[reg(+disp)]        (initial-exec TLS through a TPOFF in a register) -> left to the fault handler

  python tools/fsscan.py <elf> [--json out.json] [--show N]
"""
import argparse, collections, json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf
from funcs import fde_ranges
import capstone
from capstone import x86


def scan(e: Elf):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    md.skipdata = True
    sites = []
    for a, b in fde_ranges(e):
        try:
            o = e.off(a)
        except KeyError:
            continue
        code = e.d[o:o + (b - a)]
        if b"\x64" not in code:
            continue
        for ins in md.disasm(code, a):
            if ins.id == 0 or x86.X86_PREFIX_FS not in ins.prefix:
                continue
            mem = [op.mem for op in ins.operands if op.type == x86.X86_OP_MEM]
            if not mem:
                continue
            m = mem[0]
            kind = "abs" if m.base == 0 and m.index == 0 else "reg"
            sites.append(dict(addr=ins.address, size=ins.size, kind=kind, disp=m.disp,
                              text=f"{ins.mnemonic} {ins.op_str}", bytes=ins.bytes.hex()))
    return sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--json")
    ap.add_argument("--show", type=int, default=0)
    a = ap.parse_args()
    s = scan(Elf(a.elf))
    c = collections.Counter((x["kind"], x["disp"] if x["kind"] == "abs" else 0) for x in s)
    print(len(s), "fs sites")
    for k, n in c.most_common(30):
        print(f"  {k[0]} disp {k[1]:#x}: {n}")
    for x in [x for x in s if x["kind"] == "reg"][:a.show]:
        print(f"  {x['addr']:#x} {x['text']}")
    if a.json:
        json.dump(s, open(a.json, "w"))


if __name__ == "__main__":
    main()
