#!/usr/bin/env python3
r"""Search the game's disassembly (every FDE function) for instructions matching a regex.

  python tools/grepasm.py <regex> [--lo addr] [--hi addr]

Prints address, containing function and instruction, e.g. grepasm.py "0x60(3c|44)\]" to find stores to a field.
"""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf
from funcs import fde_ranges
from disasm import default_game
import capstone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("regex")
    ap.add_argument("--lo", type=lambda s: int(s, 16), default=0)
    ap.add_argument("--hi", type=lambda s: int(s, 16), default=1 << 64)
    a = ap.parse_args()
    rx = re.compile(a.regex)
    e = Elf(default_game())
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for lo, hi in fde_ranges(e):
        if hi <= a.lo or lo >= a.hi:
            continue
        o = e.off(lo); code = e.d[o:o + hi - lo]
        for ins in md.disasm(code, lo):
            text = f"{ins.mnemonic} {ins.op_str}"
            if rx.search(text):
                print(f"{ins.address:#x}  [{lo:#x}]  {text}")


if __name__ == "__main__":
    main()
