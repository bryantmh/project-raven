#!/usr/bin/env python3
"""Fetch the runtime libraries raven.exe loads next to the game:

- runtime/linux: the Linux C++ runtime the game links against (libstdc++.so.6, libgcc_s.so.1) from Debian 12
- runtime/windows: libopenblas.dll (OpenBLAS, BSD) for the game's LAPACK calls (src/linux/lapack.cpp)


The game's C++ code calls libstdc++ through its vtables and SysV calling convention, so it needs the real Linux
libstdc++, which the loader maps like ld.so would (docs/architecture.md#linux-libraries). Both are GPL with the GCC
Runtime Library Exception, so they can be fetched and shipped next to raven.exe.

  python tools/fetch_runtime.py [--out runtime/linux]
"""
import argparse, hashlib, io, lzma, os, tarfile, urllib.request, zipfile

POOL = "http://deb.debian.org/debian/pool/main/g/gcc-12/"
PKGS = {
    "libstdc++6_12.2.0-14+deb12u1_amd64.deb": ["libstdc++.so.6.0.30"],
    "libgcc-s1_12.2.0-14+deb12u1_amd64.deb": ["libgcc_s.so.1"],
}
NAMES = {"libstdc++.so.6.0.30": "libstdc++.so.6"}
OPENBLAS = "https://github.com/OpenMathLib/OpenBLAS/releases/download/v0.3.34/OpenBLAS-0.3.34-x64.zip"


def ar_members(blob):
    assert blob[:8] == b"!<arch>\n"
    o = 8
    while o < len(blob):
        name = blob[o:o + 16].decode().strip().rstrip("/")
        size = int(blob[o + 48:o + 58])
        yield name, blob[o + 60:o + 60 + size]
        o += 60 + size + (size & 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "runtime", "linux"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for deb, wanted in PKGS.items():
        print("fetching", deb)
        blob = urllib.request.urlopen(POOL + urllib.request.quote(deb)).read()
        for name, data in ar_members(blob):
            if not name.startswith("data.tar"):
                continue
            raw = lzma.decompress(data) if name.endswith(".xz") else data
            with tarfile.open(fileobj=io.BytesIO(raw)) as t:
                for m in t.getmembers():
                    base = os.path.basename(m.name)
                    if base in wanted and m.isfile():
                        out = os.path.join(a.out, NAMES.get(base, base))
                        body = t.extractfile(m).read()
                        open(out, "wb").write(body)
                        print("  ", out, len(body), hashlib.sha256(body).hexdigest()[:16])
    fetch_openblas(os.path.join(a.out, "..", "windows"))


def fetch_openblas(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    print("fetching", OPENBLAS)
    blob = urllib.request.urlopen(OPENBLAS).read()
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        data = z.read("bin/libopenblas.dll")
    out = os.path.join(out_dir, "libopenblas.dll")
    open(out, "wb").write(data)
    print("  ", out, len(data))


if __name__ == "__main__":
    main()
