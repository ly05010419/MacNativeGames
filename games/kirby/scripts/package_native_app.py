#!/usr/bin/env python3
import os, sys, shutil, subprocess, plistlib
from pathlib import Path

BASE = Path("/Users/vinicius/Documents/StreamHelp")
KIRBY = BASE / "Kirby_build"
APP = BASE / "Kirby and the Forgotten Land.app"

def clone(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    # Try APFS copy-on-write clone to save disk space
    res = subprocess.run(["cp", "-c", str(src), str(dst)], capture_output=True)
    if res.returncode != 0:
        shutil.copy2(src, dst)

def main():
    if APP.exists():
        shutil.rmtree(APP)

    contents = APP / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    game = resources / "game"

    macos.mkdir(parents=True)
    game.mkdir(parents=True)

    # 1. Launcher and native runtime
    shutil.copy2(KIRBY / "build/Kirby", macos / "Kirby")
    clone(KIRBY / "build/kirby-native", game / "build/kirby-native")
    os.symlink("../Resources/game/build/kirby-native", macos / "kirby-native")

    # 2. Shader tools
    clone(KIRBY / "build/shader-service/totk_xlate", game / "build/shader-service/totk_xlate")
    clone(KIRBY / "build/spirv-cross/spirv-cross", game / "build/spirv-cross/spirv-cross")

    # 3. Rewritten modules
    for mod in ["main", "subsdk0", "sdk"]:
        for ext in [".patched.bin", ".fixups.tsv", ".gates.tsv"]:
            f = KIRBY / f"rewritten/{mod}{ext}"
            if f.exists():
                clone(f, game / "rewritten" / f.name)

    # 4. RomFS
    clone(KIRBY / "romfs/romfs.bin", game / "romfs/romfs.bin")
    clone(KIRBY / "romfs/runtime-index.bin", game / "romfs/runtime-index.bin")

    # 5. Shared fonts
    fonts_dir = BASE / "TOTK_build/totk/extracted/firmware/shared-fonts"
    for font in fonts_dir.glob("*.bfttf"):
        clone(font, game / "fonts" / font.name)

    # 6. Icon
    shutil.copy2(KIRBY / "build/KirbyIcon.icns", resources / "KirbyIcon.icns")

    # 7. Precompiled shader libraries (.metallib)
    libs_dir = KIRBY / "build/shader-libs"
    if libs_dir.is_dir():
        for lib in libs_dir.glob("*.metallib"):
            clone(lib, game / "build/shader-libs" / lib.name)
        os.symlink("../Resources/game/build/shader-libs", macos / "shader-libs")
        print(f"Packaged {len(list(libs_dir.glob('*.metallib')))} precompiled shader libraries.")

    # 8. Info.plist
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "Kirby and the Forgotten Land",
        "CFBundleExecutable": "Kirby",
        "CFBundleIconFile": "KirbyIcon",
        "CFBundleIdentifier": "local.projectidea.kirby",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "Kirby",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": "26.0",
        "NSHighResolutionCapable": True,
    }
    with open(contents / "Info.plist", "wb") as f:
        plistlib.dump(info, f)

    # 8. Ad-hoc code sign
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(APP)], check=True)
    print("Successfully created and signed:", APP)

if __name__ == "__main__":
    main()
