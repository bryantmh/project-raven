#!/usr/bin/env python3
"""Inspect a Linux x86-64 ELF without section headers (the arcade binaries are stripped).

Reads everything through PT_DYNAMIC, so it works on `game`, `halo.so`, `Patch` and
any Linux .so the loader has to map.

  python tools/elfinfo.py <elf> [--imports] [--exports] [--relocs] [--json out.json]

--imports  every undefined dynamic symbol with its version and reloc kind
--exports  every defined dynamic symbol (address, size, type)
--relocs   histogram of relocation types
--json     write the full parse as JSON (used by gen_imports.py)
"""
import argparse, json, struct, sys

PT = {1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
      0x6474e550: "EH_FRAME", 0x6474e551: "STACK", 0x6474e552: "RELRO"}
DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_INIT, DT_FINI, DT_JMPREL = 10, 12, 13, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
DT_GNU_HASH, DT_VERSYM, DT_VERNEED, DT_VERNEEDNUM = 0x6ffffef5, 0x6ffffff0, 0x6ffffffe, 0x6fffffff
R_NAMES = {1: "64", 5: "COPY", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE",
           16: "DTPMOD64", 17: "DTPOFF64", 18: "TPOFF64", 37: "IRELATIVE"}


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        assert d[:4] == b"\x7fELF" and d[4] == 2, "not ELF64"
        (self.type, _, _, self.entry, phoff, _, _, _, phentsz, phnum) = struct.unpack_from(
            "<HHIQQQIHHH", d, 16)
        self.phdrs = []
        for i in range(phnum):
            p = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsz)
            self.phdrs.append(dict(type=p[0], flags=p[1], off=p[2], vaddr=p[3],
                                   filesz=p[5], memsz=p[6], align=p[7]))
        self.dyn = []
        for p in self.phdrs:
            if p["type"] == 2:
                o = p["off"]
                while True:
                    tag, val = struct.unpack_from("<qQ", d, o)
                    o += 16
                    if tag == 0:
                        break
                    self.dyn.append((tag, val))
        self.dt = {}
        for t, v in self.dyn:
            self.dt.setdefault(t, v)

    def off(self, va):
        for p in self.phdrs:
            if p["type"] == 1 and p["vaddr"] <= va < p["vaddr"] + p["filesz"]:
                return va - p["vaddr"] + p["off"]
        raise KeyError(hex(va))

    def cstr(self, va):
        o = self.off(va)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def strtab(self, i):
        return self.cstr(self.dt[DT_STRTAB] + i)

    def nsyms(self):
        if DT_HASH in self.dt:
            return struct.unpack_from("<II", self.d, self.off(self.dt[DT_HASH]))[1]
        # GNU hash: walk the chains to find the highest symbol index
        o = self.off(self.dt[DT_GNU_HASH])
        nb, symoff, bloomsz, _ = struct.unpack_from("<IIII", self.d, o)
        bk = o + 16 + bloomsz * 8
        buckets = struct.unpack_from("<%dI" % nb, self.d, bk)
        hi = max(buckets) if buckets else 0
        if hi < symoff:
            return symoff
        ch = bk + nb * 4
        while not (struct.unpack_from("<I", self.d, ch + (hi - symoff) * 4)[0] & 1):
            hi += 1
        return hi + 1

    def versions(self):
        """Map version index -> (library, version) from VERNEED."""
        out = {}
        if DT_VERNEED not in self.dt:
            return out
        o = self.off(self.dt[DT_VERNEED])
        for _ in range(self.dt.get(DT_VERNEEDNUM, 0)):
            vn_ver, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", self.d, o)
            lib = self.strtab(vn_file)
            a = o + vn_aux
            for _ in range(vn_cnt):
                _, _, vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", self.d, a)
                out[vna_other] = (lib, self.strtab(vna_name))
                a += vna_next
            if not vn_next:
                break
            o += vn_next
        return out

    def symbols(self):
        n = self.nsyms()
        so = self.off(self.dt[DT_SYMTAB])
        vers = self.versions()
        vs = self.off(self.dt[DT_VERSYM]) if DT_VERSYM in self.dt else None
        out = []
        for i in range(n):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.d, so + i * 24)
            v = struct.unpack_from("<H", self.d, vs + i * 2)[0] & 0x7FFF if vs else 0
            lib, ver = vers.get(v, ("", ""))
            out.append(dict(i=i, name=self.strtab(name) if name else "", bind=info >> 4,
                            type=info & 15, shndx=shndx, value=value, size=size, lib=lib, ver=ver))
        return out

    def relocs(self):
        out = []
        for tab, sz in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if tab not in self.dt:
                continue
            o = self.off(self.dt[tab])
            for k in range(self.dt[sz] // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", self.d, o + k * 24)
                out.append(dict(off=r_off, type=r_info & 0xFFFFFFFF, sym=r_info >> 32, addend=r_add))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--imports", action="store_true")
    ap.add_argument("--exports", action="store_true")
    ap.add_argument("--relocs", action="store_true")
    ap.add_argument("--json")
    a = ap.parse_args()
    e = Elf(a.elf)
    print("type", e.type, "entry", hex(e.entry))
    for p in e.phdrs:
        print(f"  {PT.get(p['type'], hex(p['type'])):9} off {p['off']:#x} va {p['vaddr']:#x} "
              f"filesz {p['filesz']:#x} memsz {p['memsz']:#x} flags {p['flags']}")
    for t, v in e.dyn:
        if t == DT_NEEDED:
            print("  NEEDED", e.strtab(v))
    syms = e.symbols()
    rel = e.relocs()
    kinds = {}
    for r in rel:
        if r["sym"]:
            kinds.setdefault(r["sym"], set()).add(R_NAMES.get(r["type"], str(r["type"])))
    imps = [s for s in syms if s["name"] and s["shndx"] == 0]
    exps = [s for s in syms if s["name"] and s["shndx"] != 0]
    print(f"  {len(imps)} imports, {len(exps)} exports, {len(rel)} relocs")
    if a.imports:
        for s in sorted(imps, key=lambda s: (s["lib"], s["name"])):
            print(f"  IMP {s['lib']:18} {s['ver']:14} {s['name']}  [{','.join(sorted(kinds.get(s['i'], [])))}]")
    if a.exports:
        for s in sorted(exps, key=lambda s: s["value"]):
            print(f"  EXP {s['value']:#014x} {s['size']:#8x} t{s['type']} {s['name']} {s['ver']}")
    if a.relocs:
        h = {}
        for r in rel:
            h[R_NAMES.get(r["type"], r["type"])] = h.get(R_NAMES.get(r["type"], r["type"]), 0) + 1
        print("  relocs", h)
    if a.json:
        json.dump(dict(entry=e.entry, phdrs=e.phdrs, imports=imps, exports=exps, relocs=rel,
                       needed=[e.strtab(v) for t, v in e.dyn if t == DT_NEEDED]),
                  open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
