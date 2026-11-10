"""Builds the Linux AppImage's folder (AppDir) and, with appimagetool, the AppImage (docs/linux.md).

    python3 -I packaging/linux/build_appimage.py --zig DIR --cmake DIR --ninja FILE --sdl3 DIR [--appimagetool FILE]

DIR are unpacked official releases: Zig (ziglang.org), CMake (Kitware's linux-x86_64 archive), Ninja (ninja-linux.zip);
--sdl3 is an SDL3 built and installed beforehand (cmake --install, see docs/linux.md). The game sources come from git
(tracked files and new ones not ignored, never game data), the pinned third-party sources from build/deps/, so that the
player's PC needs no network to build. Writes build/appimage/AppDir and, with --appimagetool, build/appimage/*.AppImage.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "appimage"
SOURCES = ["CMakeLists.txt", "LICENSE", "cmake", "runtime", "recomp", "re/out/psp_nids_ppsspp.tsv", "re/out/recomp/*.recomp.json",
           "tools/launcher.pyw", "tools/toolchain.py", "tools/steam_shortcuts.py", "tools/launcher_assets/icon.png",
           "tools/launcher_assets/Cinzel[[]wght].ttf", "tools/launcher_assets/OFL.txt", "roms"]
DEPS = ["at3_standalone", "openh264", "libkirk", "ppsspp_prx", "xbrz", "stb", "libchdr-*"]  # SDL3 goes in prebuilt
BANNED = (".chd", ".iso", ".cso", ".pbp", ".prx", ".bin")
# Zig only ever builds for x86_64-linux-gnu.2.38 here (tools/toolchain.py): leave out the other targets' libc sources
# and headers, its docs and tsan (406 -> 246 MB unpacked; the games build byte-identical with what is left)
ZIG_DROP = {".": {"doc"}, "lib": {"docs", "libtsan"},
            "lib/libc": {"musl", "mingw", "wasi", "darwin", "freebsd", "netbsd", "openbsd"}}
ZIG_INCLUDE_KEEP = {"generic-glibc", "any-linux-any", "x86-linux-any", "x86-linux-gnu"}


def git_files():
    r = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "--"] + SOURCES, cwd=ROOT,
                       capture_output=True, text=True, check=True)
    files = sorted(set(r.stdout.split("\n")) - {""})
    bad = [f for f in files if f.lower().endswith(BANNED) or f.startswith(("build/", "media/", "re/dumps/", "re/ghidra/"))]
    if bad or not files:
        sys.exit(f"refusing to bundle: {bad or 'no files'}")
    return files


def dep_files():
    out = []
    for pat in DEPS:
        for d in sorted((ROOT / "build" / "deps").glob(pat)):
            out += [p.relative_to(ROOT).as_posix() for p in sorted(d.rglob("*")) if p.is_file()]
    if not any("libchdr" in f for f in out):
        sys.exit("build/deps has no libchdr: configure any build once so CMake downloads the pinned sources")
    return out


def zig_ignore(root):
    def ignore(d, names):
        rel = Path(os.path.relpath(d, root)).as_posix()
        if rel == "lib/libc/include":
            missing = ZIG_INCLUDE_KEEP - set(names)
            if missing:
                sys.exit(f"this Zig has no {sorted(missing)} in lib/libc/include: update ZIG_INCLUDE_KEEP")
            return set(names) - ZIG_INCLUDE_KEEP
        return ZIG_DROP.get(rel, set()) & set(names)
    return ignore


def main():
    ap = argparse.ArgumentParser()
    for k in ("zig", "cmake", "ninja", "sdl3"):
        ap.add_argument("--" + k, required=True)
    ap.add_argument("--appimagetool")
    a = ap.parse_args()
    app = OUT / "AppDir"
    if app.exists():
        shutil.rmtree(app)
    src = app / "usr" / "share" / "gow-psp" / "src"
    lines = []
    for f in git_files() + dep_files():
        data = (ROOT / f).read_bytes()
        (src / f).parent.mkdir(parents=True, exist_ok=True)
        (src / f).write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {f}")
    (src / "manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    lib = app / "usr" / "lib" / "gow-psp"
    shutil.copytree(a.zig, lib / "zig", symlinks=True, ignore=zig_ignore(a.zig))
    shutil.copytree(a.cmake, lib / "cmake", symlinks=True, ignore=shutil.ignore_patterns("doc", "man"))
    (lib / "ninja").mkdir(parents=True)
    shutil.copy2(a.ninja, lib / "ninja" / "ninja")
    shutil.copytree(a.sdl3, lib / "sdl3", symlinks=True)
    (lib / "sdl3" / "VERSION").write_text(hashlib.sha256((Path(a.sdl3) / "lib" / "libSDL3.a").read_bytes()).hexdigest() + "\n")
    here = Path(__file__).resolve().parent
    shutil.copy2(here / "AppRun", app / "AppRun")
    shutil.copy2(here / "gow-psp.desktop", app / "gow-psp.desktop")
    shutil.copy2(ROOT / "tools" / "launcher_assets" / "icon.png", app / "gow-psp.png")
    print(f"AppDir: {app} ({len(lines)} source files)")
    if a.appimagetool:
        target = OUT / "God_of_War_PSP-x86_64.AppImage"
        subprocess.run([a.appimagetool, "--no-appstream", str(app), str(target)], check=True, env={"ARCH": "x86_64", "PATH": "/usr/bin:/bin", "APPIMAGE_EXTRACT_AND_RUN": "1"})
        print(f"AppImage: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
