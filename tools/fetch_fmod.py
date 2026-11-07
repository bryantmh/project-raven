#!/usr/bin/env python3
"""Copy the FMOD Studio API 1.10.20 runtime DLLs into runtime/windows, from the official Windows installer.

  python tools/fetch_fmod.py <fmodstudioapi11020win-installer.exe> [--7z "C:/Program Files/7-Zip/7z.exe"]

Download the installer from https://www.fmod.com/download (FMOD Engine, version 1.10.20, Windows; needs a free
account). The installer is opened with 7-Zip like an archive, nothing is installed. The build embeds fmod64.dll and
fmodstudio64.dll in raven.exe (tools/pack_runtime.py). FMOD's license allows this for non-commercial projects that
credit "FMOD Studio by Firelight Technologies Pty Ltd." (shown in the settings menu).
"""
import argparse, os, shutil, subprocess, sys, tempfile

WANTED = {"api/lowlevel/lib/fmod64.dll": "fmod64.dll", "api/studio/lib/fmodstudio64.dll": "fmodstudio64.dll"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("installer")
    ap.add_argument("--7z", dest="sevenzip", default=r"C:\Program Files\7-Zip\7z.exe")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "runtime", "windows"))
    a = ap.parse_args()
    if not os.path.exists(a.sevenzip):
        sys.exit("7-Zip not found; pass --7z <path to 7z.exe>")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([a.sevenzip, "x", "-y", "-o" + tmp, a.installer] + [k.replace("/", os.sep) for k in WANTED],
                       check=True, stdout=subprocess.DEVNULL)
        os.makedirs(a.out, exist_ok=True)
        for src, dst in WANTED.items():
            shutil.copyfile(os.path.join(tmp, src), os.path.join(a.out, dst))
            print("fetch_fmod:", dst)


if __name__ == "__main__":
    main()
