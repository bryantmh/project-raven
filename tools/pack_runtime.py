#!/usr/bin/env python3
"""Pack the runtime files raven.exe needs into one compressed blob that is embedded in the exe (src/runtime_pack.rc).

  python tools/pack_runtime.py <out.pak> <root> <relative path> [<relative path> ...]

The blob is Windows' LZMS compression (cabinet.dll Compression API, the same one raven.exe decompresses with) of:
  "RVPAK1" u32 count, then per file: u16 path length, UTF-8 path ('/' separated), u64 size, bytes.
raven.exe unpacks it once into %LOCALAPPDATA%\\ProjectRaven\\runtime\\<hash> (src/app/runtime_pack.cpp).
"""
import ctypes, ctypes.wintypes as wt, os, struct, sys

COMPRESS_ALGORITHM_LZMS = 5


def lzms(data: bytes) -> bytes:
    cab = ctypes.WinDLL("cabinet")
    handle = ctypes.c_void_p()
    if not cab.CreateCompressor(COMPRESS_ALGORITHM_LZMS, None, ctypes.byref(handle)):
        raise OSError("CreateCompressor failed")
    try:
        cap = len(data) + 65536
        out = ctypes.create_string_buffer(cap)
        size = ctypes.c_size_t()
        if not cab.Compress(handle, data, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(cap), ctypes.byref(size)):
            raise OSError("Compress failed: %d" % ctypes.GetLastError())
        return out.raw[:size.value]
    finally:
        cab.CloseCompressor(handle)


def main():
    out, root, rels = sys.argv[1], sys.argv[2], sys.argv[3:]
    blob = bytearray(b"RVPAK1" + struct.pack("<I", len(rels)))
    for rel in rels:
        data = open(os.path.join(root, rel), "rb").read()
        name = rel.replace("\\", "/").encode()
        blob += struct.pack("<H", len(name)) + name + struct.pack("<Q", len(data)) + data
    packed = lzms(bytes(blob))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(packed)
    print("pack_runtime: %d files, %.1f MB -> %.1f MB" % (len(rels), len(blob) / 1e6, len(packed) / 1e6))


if __name__ == "__main__":
    main()
