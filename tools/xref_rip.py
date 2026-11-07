#!/usr/bin/env python3
"""Find instructions that reference given absolute addresses through rip-relative operands.

  python tools/xref_rip.py <addr> [addr ...]
"""
import sys, re; sys.path.insert(0,'tools')
from elfinfo import Elf
from funcs import fde_ranges
from disasm import default_game
import capstone
targets={int(x,16) for x in sys.argv[1:]}
e=Elf(default_game()); md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for lo,hi in fde_ranges(e):
    o=e.off(lo); code=e.d[o:o+hi-lo]
    for ins in md.disasm(code, lo):
        m=re.search(r'rip \+ (0x[0-9a-f]+)', ins.op_str)
        if m and ins.address+ins.size+int(m.group(1),16) in targets:
            print(hex(ins.address), hex(lo), ins.mnemonic, ins.op_str)
