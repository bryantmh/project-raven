#!/usr/bin/env python3
"""Disassemble a range of the game (or any section-less ELF) with imports named.

  python tools/disasm.py <addr> [count] [--elf path] [--bytes N]

Default ELF is the game (<game>/halo/game, from RAVEN_GAME or the config's game_folder). Calls through GOT slots
are annotated with the import name from the embedded import table (docs/architecture.md#the-embedded-import-table)
and the PLT, and string operands (immediates pointing at C strings) are shown.
"""
import argparse, json, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf
import capstone

TABLE, STRBASE = 0x1F4D938, 0x1F492CC


def default_game():
    if os.environ.get("RAVEN_GAME"):
        return os.path.join(os.environ["RAVEN_GAME"], "halo", "game")
    cfg = os.path.join(os.environ.get("APPDATA", ""), "ProjectRaven", "config.json")
    if os.path.exists(cfg):
        return os.path.join(json.load(open(cfg))["game_folder"], "halo", "game")
    sys.exit("set RAVEN_GAME")


def got_names(e):
    names = {}
    try:
        o = e.off(TABLE)
        while struct.unpack_from("<I", e.d, o + 0x20)[0] in (6, 7):
            a, b, got = struct.unpack_from("<IIQ", e.d, o)
            names[got] = e.cstr(STRBASE + a)
            o += 0x24
    except KeyError:
        pass
    syms = e.symbols()
    for r in e.relocs():
        if r["sym"] and r["type"] in (6, 7):
            names[r["off"]] = syms[r["sym"]]["name"]
    return names


def plt_targets(e, names):
    """PLT stub address -> import name (stubs are `jmp [rip+X]`)."""
    out = {}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    seg = [p for p in e.phdrs if p["type"] == 1][0]
    d = e.d
    # scan for ff 25 (jmp qword [rip+disp32]) in the first segment's code
    for m in re.finditer(rb"\xff\x25", d[seg["off"]:seg["off"] + seg["filesz"]]):
        va = seg["vaddr"] + m.start()
        disp = struct.unpack_from("<i", d, seg["off"] + m.start() + 2)[0]
        tgt = va + 6 + disp
        if tgt in names:
            out[va] = names[tgt]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("count", nargs="?", type=int, default=40)
    ap.add_argument("--elf")
    a = ap.parse_args()
    e = Elf(a.elf or default_game())
    names = got_names(e)
    plt = plt_targets(e, names)
    addr = int(a.addr, 16)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    o = e.off(addr)
    n = 0
    for ins in md.disasm(e.d[o:o + a.count * 15], addr):
        note = ""
        m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
        if m:
            tgt = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == "+" else -1))
            note = f"[{tgt:#x}" + (f" {names[tgt]}" if tgt in names else "") + "]"
        for imm in re.findall(r"\b0x([0-9a-f]{5,8})\b", ins.op_str):
            v = int(imm, 16)
            if v in plt:
                note += f" <{plt[v]}>"
            else:
                try:
                    s = e.cstr(v)
                    if 2 <= len(s) < 80 and s.isprintable():
                        note += f' "{s}"'
                except (KeyError, ValueError):
                    pass
        print(f"{ins.address:#010x}  {ins.mnemonic:8} {ins.op_str:45} {note}")
        n += 1
        if n >= a.count:
            break


if __name__ == "__main__":
    main()
