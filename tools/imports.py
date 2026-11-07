#!/usr/bin/env python3
"""Every function and variable the game and the Linux libraries it loads import, and which ones the shim layer
implements.

Sources: the game's dynamic relocations, the game's embedded protector import table
(docs/architecture.md#the-embedded-import-table), and the imports of runtime/linux/*.so and halo.so that no other
loaded ELF exports. Implemented names are the ones registered with SHIM/SHIM_VAR/SHIM_ALIAS in src/.

  python tools/imports.py <game dir>/halo [--missing] [--by-lib]
"""
import argparse, glob, os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from elfinfo import Elf

ROOT = os.path.join(os.path.dirname(__file__), "..")
TABLE, STRBASE = 0x1F4D938, 0x1F492CC


def embedded_imports(e):
    o = e.off(TABLE)
    out = []
    while struct.unpack_from("<I", e.d, o + 0x20)[0] in (6, 7):
        a, b, got = struct.unpack_from("<IIQ", e.d, o)
        out.append((e.cstr(STRBASE + a), e.cstr(STRBASE + b), got))
        o += 0x24
    return out


def implemented():
    names = set()
    for f in glob.glob(os.path.join(ROOT, "src", "**", "*.*"), recursive=True):
        if f.endswith((".cpp", ".h", ".inc")):
            for m in re.finditer(r"\bSHIM(?:_VAR|_ALIAS|_V|_STUB)?\(\s*[^,()]*?,?\s*(\w+)", open(f, encoding="utf-8").read()):
                names.add(m.group(1))
            for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*(?:\(void\s*\*\)|reinterpret_cast)', open(f, encoding="utf-8").read()):
                names.add(m.group(1))
    return names


def collect(halo_dir):
    need = {}  # name -> set of (importer, version)
    g = Elf(os.path.join(halo_dir, "game"))
    for s in g.symbols():
        if s["name"] and s["shndx"] == 0:
            need.setdefault(s["name"], set()).add(("game", s["ver"]))
    for n, v, _ in embedded_imports(g):
        need.setdefault(n, set()).add(("game*", v))
    libs = [Elf(p) for p in glob.glob(os.path.join(ROOT, "runtime", "linux", "*.so*"))]
    libs.append(Elf(os.path.join(halo_dir, "halo.so")))
    exported = set()
    for e in libs:
        exported |= {s["name"] for s in e.symbols() if s["name"] and s["shndx"] != 0}
    for e, nm in zip(libs, [os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "runtime", "linux", "*.so*"))] + ["halo.so"]):
        for s in e.symbols():
            if s["name"] and s["shndx"] == 0 and s["bind"] != 2:  # weak undefined may stay null
                need.setdefault(s["name"], set()).add((nm, s["ver"]))
    return need, exported


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("halo_dir")
    ap.add_argument("--missing", action="store_true")
    a = ap.parse_args()
    need, exported = collect(a.halo_dir)
    have = implemented()
    rows = []
    for n, who in sorted(need.items()):
        src = "shim" if n in have else ("elf" if n in exported else "MISSING")
        rows.append((src, n, sorted(who)))
    cnt = {}
    for r in rows:
        cnt[r[0]] = cnt.get(r[0], 0) + 1
    print(cnt)
    for src, n, who in rows:
        if not a.missing or src == "MISSING":
            print(f"{src:7} {n:50} {' '.join(w + ('@' + v if v else '') for w, v in who)}")


if __name__ == "__main__":
    main()
