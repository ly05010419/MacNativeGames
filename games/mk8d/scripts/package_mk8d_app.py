#!/usr/bin/env python3
"""Build Mario Kart 8 Deluxe.app: a self-contained, locally signed Mac app around the native runtime (adapted from Odyssey's).

    make app WORK=<work> [APP=/Applications/Mario\\ Kart\\ 8\\ Deluxe.app] [DATA=<folder>]

The bundle carries the runtime, the shader tools, the rewritten modules, the merged RomFS and the shared fonts. Large files
are APFS clones of the work folder's, so the app takes no extra space when it is on the same volume. Saves, the shader
cache and the logs go to the data folder (scripts/mk8d_app_launcher.c), by default "MK8D Data" in the work folder. The app
contains game data from your own dump: never share it. On first packaging the data folder is seeded with `make run`'s save
and shader cache (<work>/play); existing data is never replaced, so the app and `make run` keep separate saves after that.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
from pathlib import Path

from mk8d import GAME as GAME_DIR, VERSION

NAME = "Mario Kart 8 Deluxe"


def clone(source: Path, target: Path):
    """APFS copy-on-write clone (no extra space); plain copy elsewhere."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(["cp", "-c", str(source), str(target)]).returncode != 0:
        shutil.copy2(source, target)


def seed_data(data: Path, play: Path):
    """`make run`'s save, pipeline list and shader cache, once: a data folder with a save is left as it is."""
    data.mkdir(parents=True, exist_ok=True)
    profile = data / "profile"
    if (profile / "save").is_dir():
        print(f"{data.name} already has a save; left as it is.")
    elif (play / "profile/save").is_dir():
        subprocess.run(["cp", "-cR", str(play / "profile"), str(profile)], check=True)
        shutil.rmtree(profile / "temp", ignore_errors=True)
        print(f"Seeded the save from {play / 'profile'}.")
    cache = data / "shader-cache"
    if not cache.exists() and (play / "shader-cache").is_dir():
        subprocess.run(["cp", "-cR", str(play / "shader-cache"), str(cache)], check=True)
        print("Seeded the shader cache.")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", type=Path, required=True, help="the build folder (scripts/build_mk8d.py --work)")
    p.add_argument("--app", type=Path, help=f'the app to write (default: <work>/{NAME}.app)')
    p.add_argument("--data", type=Path, help='saves, shader cache and logs (default: <work>/MK8D Data)')
    args = p.parse_args()
    work = args.work.expanduser().resolve()
    app = (args.app or work / f"{NAME}.app").expanduser().resolve()
    data = (args.data or work / "MK8D Data").expanduser().resolve()
    if app.suffix != ".app":
        raise SystemExit(f"--app must end in .app: {app}")
    checkout = GAME_DIR.parents[1]
    for path in (app, data):
        if checkout in path.parents:
            raise SystemExit(f"{path} holds game data: keep it outside the checkout")
    native, icon = work / "build/mk8d-native", work / "app-assets/icon.jpg"
    for need in (native, icon, work / "build/shader-service/totk_xlate", work / "build/spirv-cross/spirv-cross",
                 work / "extracted/local/romfs/romfs.bin", work / "extracted/firmware/shared-fonts"):
        if not need.exists():
            raise SystemExit(f"missing {need}; run scripts/build_mk8d.py first (it makes the runtime and the icon)")

    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"; macos = contents / "MacOS"; resources = contents / "Resources"; game = resources / "game"
    macos.mkdir(parents=True)
    clone(native, game / "build/mk8d-native")
    clone(work / "build/shader-service/totk_xlate", game / "build/shader-service/totk_xlate")
    clone(work / "build/spirv-cross/spirv-cross", game / "build/spirv-cross/spirv-cross")
    for item in (work / "rewritten").iterdir():
        if item.is_file() and not item.is_symlink():
            clone(item, game / "rewritten" / item.name)
    os.symlink("../rewritten", game / "rewritten/local")   # the module directory the runtime is given, as in the work folder
    for name in ("romfs.bin", "runtime-index.bin"):
        clone(work / "extracted/local/romfs" / name, game / "romfs" / name)
    for font in sorted((work / "extracted/firmware/shared-fonts").glob("*.bfttf")):
        clone(font, game / "fonts" / font.name)
    os.symlink("../Resources/game/build/mk8d-native", macos / "mk8d-native")
    # The whole-game shader seed (scripts/prepare_mk8d_shader_seed.py): the runtime imports build/shader-seed beside its
    # real executable (totk_shader_rt.m root_dir, symlinks resolved) into the data folder's cache on the first start,
    # checking every file against seed-manifest.json; later starts only compare the receipt.
    seed = work / "build/shader-seed"
    if (seed / "seed-manifest.json").is_file():
        subprocess.run(["cp", "-cR", str(seed), str(game / "build/shader-seed")], check=True)
    # Its Metal libraries (scripts/precompile_shader_libs.py). The runtime looks for them beside the executable path it was
    # started by (nvn_mtl.m exe_rel, not resolved), which here is Contents/MacOS, so they are linked there too.
    libs = work / "build/shader-libs"
    if any(libs.glob("*.metallib")):   # one folder clone: tens of thousands of files
        subprocess.run(["cp", "-cR", str(libs), str(game / "build/shader-libs")], check=True)
        os.symlink("../Resources/game/build/shader-libs", macos / "shader-libs")
    subprocess.run(["xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(macos / "MK8D"),
                    str(GAME_DIR / "scripts/mk8d_app_launcher.c"), "-framework", "CoreFoundation"], check=True)
    subprocess.run(["sips", "-s", "format", "icns", str(icon), "--out", str(resources / "MK8DIcon.icns")],
                   check=True, stdout=subprocess.DEVNULL)
    (resources / "game-version.txt").write_text(VERSION + "\n")
    (resources / "data-dir.txt").write_text(str(data) + "\n")
    info = {
        "CFBundleDevelopmentRegion": "en", "CFBundleDisplayName": NAME, "CFBundleName": NAME,
        "CFBundleExecutable": "MK8D", "CFBundleIconFile": "MK8DIcon",
        "CFBundleIdentifier": "local.macnativegames.mk8d", "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.1", "CFBundleVersion": "1",
        "LSApplicationCategoryType": "public.app-category.games", "LSMinimumSystemVersion": "26.0",
        "NSHighResolutionCapable": True,
    }
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump(info, file)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, stdout=subprocess.DEVNULL)
    seed_data(data, work / "play")
    print(f"{app}\n  data: {data}\nIt holds game data from your own dump: do not share it.")


if __name__ == "__main__":
    main()
