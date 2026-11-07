"""Build a commit and publish it as a GitHub release. Called by tools/hooks/pre-push when master is pushed.

Usage: py tools/release.py <remote> <sha> [<previous remote sha>]
       py tools/release.py --build-only [<sha>]   (build and package only: nothing is pushed or published)

The commit is exported to .release/src (only changed files are rewritten, so rebuilds stay incremental) and built
in .release/build, so uncommitted changes in the working tree never end up in a release. The runtime files aren't in
git (runtime/linux from tools/fetch_runtime.py, runtime/windows from tools/fetch_fmod.py and fetch_runtime.py): they
are copied from the working tree. The fetched dependencies (SDL2, Dear ImGui, mimalloc) are reused from build/_deps
when the working tree's build has them, so the release build doesn't download them again.

After a successful build the commit is pushed (the push that triggered the hook then has nothing left to do), and
raven.exe is uploaded as a release. VERSION holds the major.minor version and is only changed by hand: the first
release of a version is tagged v<major>.<minor>, and each later one gets the next patch number, up to .999.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / ".release"
SRC = WORK / "src"
BUILD = WORK / "build"
RUNTIME_DIRS = ["runtime/linux", "runtime/windows"]
RUNTIME_FILES = ["runtime/linux/libstdc++.so.6", "runtime/linux/libgcc_s.so.1", "runtime/windows/libopenblas.dll",
                 "runtime/windows/fmod64.dll", "runtime/windows/fmodstudio64.dll"]
DEPS = {"SDL2": "sdl2-src", "IMGUI": "imgui-src", "MIMALLOC": "mimalloc-src"}
LLVM = "C:/Program Files/LLVM/bin"  # CMake wants forward slashes
NULL_SHA = "0" * 40
MAX_PATCH = 999


def git(*args, cwd=ROOT):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def run(cmd, cwd):
    print(f"> {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def export(sha):
    """Writes the commit's files into SRC, leaving unchanged files (and their timestamps) alone."""
    archive = subprocess.run(["git", "archive", "--format=tar", sha], cwd=ROOT, check=True, capture_output=True).stdout
    wanted = set()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            path = SRC / member.name
            wanted.add(path)
            data = tar.extractfile(member).read()
            if path.is_file() and path.read_bytes() == data:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    # Remove files the commit no longer has, skipping the copied runtime folders.
    keep = {SRC / d for d in RUNTIME_DIRS}
    for dirpath, dirnames, filenames in os.walk(SRC, topdown=False):
        folder = Path(dirpath)
        if any(folder == k or k in folder.parents for k in keep):
            continue
        for name in filenames:
            path = folder / name
            if path not in wanted:
                path.unlink()
        if folder != SRC and not any(folder.iterdir()):
            folder.rmdir()

    for name in RUNTIME_FILES:
        if not (ROOT / name).is_file():
            raise SystemExit(f"{name} is missing; run tools/fetch_runtime.py and tools/fetch_fmod.py first")
        (SRC / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, SRC / name)


def build():
    if not (BUILD / "build.ninja").exists():
        cmd = ["cmake", "-S", str(SRC), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
               f"-DCMAKE_C_COMPILER={LLVM}/clang.exe", f"-DCMAKE_CXX_COMPILER={LLVM}/clang++.exe",
               f"-DCMAKE_RC_COMPILER={LLVM}/llvm-rc.exe"]
        for name, folder in DEPS.items():
            dep = ROOT / "build" / "_deps" / folder
            if dep.is_dir():
                cmd.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={dep.as_posix()}")
        run(cmd, ROOT)
    run(["cmake", "--build", str(BUILD), "--target", "raven"], ROOT)
    exe = BUILD / "raven.exe"
    if not exe.is_file():
        raise SystemExit("The build didn't produce raven.exe")
    return exe


def github_repo(remote):
    url = git("remote", "get-url", remote)
    m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?/?$", url)
    if not m:
        raise SystemExit(f"Remote {remote} ({url}) isn't a GitHub repo")
    return m.group(1)


def next_tag(repo, sha):
    """Returns the tag for the next release of the major.minor version in the commit's VERSION file."""
    base = git("show", f"{sha}:VERSION")
    if not re.fullmatch(r"\d+\.\d+", base):
        raise SystemExit(f"VERSION must be <major>.<minor>, not {base!r}")
    tags = subprocess.run(["gh", "release", "list", "--repo", repo, "--limit", "10000", "--json", "tagName",
                           "--jq", ".[].tagName"], check=True, capture_output=True, text=True).stdout.split()
    patches = [int(m.group(1) or 0) for t in tags if (m := re.fullmatch(rf"v{re.escape(base)}(?:\.(\d+))?", t))]
    if not patches:
        return f"v{base}"
    patch = max(patches) + 1
    if patch > MAX_PATCH:
        raise SystemExit(f"v{base} has used all {MAX_PATCH} patch versions; bump the minor or major version in VERSION")
    return f"v{base}.{patch}"


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--build-only":
        sha = git("rev-parse", sys.argv[2] if len(sys.argv) > 2 else "HEAD")
        SRC.mkdir(parents=True, exist_ok=True)
        export(sha)
        print(f"Built {build()} from {sha[:7]}")
        return
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    remote, sha = sys.argv[1], git("rev-parse", sys.argv[2])
    previous = sys.argv[3] if len(sys.argv) == 4 and sys.argv[3] != NULL_SHA else None
    repo = github_repo(remote)
    tag = next_tag(repo, sha)

    SRC.mkdir(parents=True, exist_ok=True)
    export(sha)
    exe = build()
    asset = WORK / "raven.exe"
    shutil.copy2(exe, asset)

    log_range = [f"{previous}..{sha}"] if previous else ["-1", sha]
    changes = git("log", "--format=- %s", *log_range)
    notes = ("Download `raven.exe`, put it in any folder and run it. You need your own copy of the Halo: Fireteam Raven "
             "arcade files: the folder that contains both `halo` and `dump`.\n\n"
             f"## Changes\n\n{changes}\n")

    # Push the commit now so the release tag can point at it; the outer push then finds master up to date.
    run(["git", "push", "--no-verify", remote, f"{sha}:refs/heads/master"], ROOT)
    run(["gh", "release", "create", tag, str(asset), "--repo", repo, "--target", sha,
         "--title", f"Project Raven {tag[1:]} ({sha[:7]})", "--notes", notes, "--latest"], ROOT)
    asset.unlink()


if __name__ == "__main__":
    main()
