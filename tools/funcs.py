#!/usr/bin/env python3
"""Function ranges of a stripped Linux ELF, read from its .eh_frame_hdr / .eh_frame.

Every function compiled with unwind tables (all of the game's C++ code, libstdc++, libgcc_s) has an FDE, so this
gives the code ranges to disassemble without guessing where code ends and data starts.

  python tools/funcs.py <elf>            prints the count and the first ranges
  import: from funcs import fde_ranges   -> sorted list of (start, end)
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf


def _uleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7F) << s; s += 7
        if b < 0x80:
            return r, o


def _sleb(d, o):
    r = s = 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7F) << s; s += 7
        if b < 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, o


def _enc(d, o, enc, va_of):
    """Read a DW_EH_PE encoded pointer at file offset o. va_of(o) maps a file offset to its address (pcrel)."""
    if enc == 0xFF:
        return None, o
    fmt = enc & 0x0F
    if fmt == 0x00:
        v = struct.unpack_from("<Q", d, o)[0]; n = 8
    elif fmt == 0x03:
        v = struct.unpack_from("<I", d, o)[0]; n = 4
    elif fmt == 0x0B:
        v = struct.unpack_from("<i", d, o)[0]; n = 4
    elif fmt == 0x0C:
        v = struct.unpack_from("<q", d, o)[0]; n = 8
    elif fmt == 0x01:
        v, o2 = _uleb(d, o); return v, o2
    elif fmt == 0x09:
        v, o2 = _sleb(d, o); return v, o2
    else:
        raise ValueError("pointer encoding %#x" % enc)
    if enc & 0x70 == 0x10:
        v += va_of(o)
    return v & 0xFFFFFFFFFFFFFFFF, o + n


def fde_ranges(e: Elf):
    hdr = [p for p in e.phdrs if p["type"] == 0x6474E550]
    if not hdr:
        return []
    h = hdr[0]
    d = e.d
    ho = h["off"]
    va_of = lambda o: o - ho + h["vaddr"]  # offsets near the hdr share its segment
    ver, eh_ptr_enc, fde_count_enc, table_enc = d[ho:ho + 4]
    eh_frame, o = _enc(d, ho + 4, eh_ptr_enc, va_of)
    # Walk .eh_frame itself (more complete than the hdr table if a protector rebuilt it).
    o = e.off(eh_frame)
    seg = [p for p in e.phdrs if p["type"] == 1 and p["vaddr"] <= eh_frame < p["vaddr"] + p["filesz"]][0]
    va_of = lambda off: off - seg["off"] + seg["vaddr"]
    cies = {}
    out = []
    while True:
        ln = struct.unpack_from("<I", d, o)[0]
        if ln == 0:
            break
        start = o + 4
        if ln == 0xFFFFFFFF:
            raise ValueError("64-bit eh_frame")
        cie_id = struct.unpack_from("<I", d, start)[0]
        if cie_id == 0:
            # CIE: version, augmentation, code/data align, ra reg, aug data
            p = start + 5
            aug = d[p:d.index(b"\0", p)].decode(); p += len(aug) + 1
            _, p = _uleb(d, p); _, p = _sleb(d, p); _, p = _uleb(d, p)
            fenc = 0
            if aug.startswith("z"):
                _, p = _uleb(d, p)
                for c in aug[1:]:
                    if c == "R":
                        fenc = d[p]; p += 1
                    elif c == "P":
                        penc = d[p]; p += 1
                        _, p = _enc(d, p, penc, va_of)
                    elif c == "L":
                        p += 1
            cies[o] = fenc
        else:
            cie = start - cie_id
            fenc = cies.get(cie, 0x1B)
            pc, p = _enc(d, start + 4, fenc, va_of)
            rng, p = _enc(d, p, fenc & 0x0F, va_of)
            if rng:
                out.append((pc, pc + rng))
        o = start + ln
    out.sort()
    return out


if __name__ == "__main__":
    e = Elf(sys.argv[1])
    r = fde_ranges(e)
    print(len(r), "functions", sum(b - a for a, b in r), "bytes")
    for a, b in r[:10]:
        print(hex(a), hex(b))
