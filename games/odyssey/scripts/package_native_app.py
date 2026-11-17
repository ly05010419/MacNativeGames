#!/usr/bin/env python3
"""Build Odyssey.app: a self-contained, locally signed Mac app around the native runtime.

The bundle carries the runtime, shader tools, rewritten modules, merged RomFS and shared fonts (large files are APFS
clones, so they take no extra space). Saves, the shader cache and the log go to "Odyssey Data" next to the app
(scripts/odyssey_app_launcher.c). Both stay in the ignored Odyssey_build folder; the app contains game data from the
user's own dump and must not be shared. On first packaging "Odyssey Data" is seeded with the newest local save
(the test app's profile or a baseline) and the warmed shader cache; existing data is never replaced.
"""
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("ODYSSEY_WORK", GAME / "Odyssey_build")).resolve()   # ODYSSEY_WORK: another build folder (scripts/build_odyssey.py)
APP = Path(os.environ.get("ODYSSEY_APP", BUILD / "Odyssey.app")).resolve()   # ODYSSEY_APP: MNG Launcher's output (games/odyssey/build.py)
BUNDLE_ID = "org.macnativegames.odyssey"   # the other ports' form; tools/builder_jobs/installed.py finds the app by it
# The player's data (odyssey_app_launcher.c): Application Support/MacNativeGames/Odyssey, as the other ports keep theirs. An
# "Odyssey Data" folder next to an app packaged before 2026-11-17 is moved there by the launcher at its first start.
DATA = Path.home() / "Library/Application Support/MacNativeGames/Odyssey"
OLD_DATA = APP.parent / "Odyssey Data"
ICON = BUILD / "app-assets/OdysseyIcon.jpg"
CHAIN = "rt-totk-native-v4-unverified"


def clone(source: Path, target: Path):
    """APFS copy-on-write clone (no extra space); plain copy elsewhere."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if subprocess.run(["cp", "-c", str(source), str(target)]).returncode != 0:
        shutil.copy2(source, target)


def save_time(profile: Path) -> float:
    files = [f for f in (profile / "save").rglob("File1.bin")] if (profile / "save").is_dir() else []
    return max((f.stat().st_mtime for f in files), default=0)


def seed_data():
    if not DATA.exists() and OLD_DATA.is_dir():
        print(f"{OLD_DATA} is moved to {DATA} when the app starts; nothing seeded.")
        return
    DATA.mkdir(parents=True, exist_ok=True)
    profile = DATA / "profile"
    if save_time(profile):
        print("Odyssey Data already has a save; left as it is.")
    else:
        candidates = [BUILD / "manual-test/profile", BUILD / "profiles/bonneton-cappy", BUILD / "profiles/cap-kingdom-start"]
        best = max(candidates, key=save_time)
        if save_time(best):
            profile.mkdir(mode=0o700, exist_ok=True)
            shutil.copy2(best / "account.uid", profile / "account.uid")
            # The seeded identity is the user "Local" (runtime/account.c): a users.txt left by an earlier run would name
            # another user and hide this save, so it is kept aside.
            if (profile / "users.txt").is_file():
                (profile / "users.txt").replace(profile / "users.txt.before-seed")
            shutil.copytree(best / "save", profile / "save", dirs_exist_ok=True)
            print(f"Seeded the save from {best.relative_to(BUILD)}.")
    cache = DATA / "shader-cache"
    if not cache.exists() and (BUILD / "build/shader-cache").is_dir():
        subprocess.run(["cp", "-cR", str(BUILD / "build/shader-cache"), str(cache)], check=True)
        print("Seeded the shader cache.")
    minutes = DATA / "session-minutes.txt"
    if not minutes.exists():
        minutes.write_text("20\n")


def package_warm_lists(macos: Path, game: Path):
    """Ship Odyssey's pipeline recipes alongside the libraries, resolving seed variant aliases."""
    tool = GAME / "third_party/totk-scripts/shader_list.py"
    manifest = BUILD / "build/pso_manifest.tsv"
    shared_title = BUILD / "build/pso_shared_title.tsv"
    subprocess.run([sys.executable, str(tool), "export", str(GAME / "assets/shader_list.tsv"),
                    str(manifest), str(shared_title)], check=True)
    title = BUILD / "build/pso_title.tsv"
    if not title.is_file():
        title = shared_title
    # The seed deduplicates variants with identical dependencies. Recorded pipeline keys must name the surviving
    # variants, as in build_app.py place_runtime, or the warm-up silently skips otherwise available pipelines.
    aliases = BUILD / "build/shader-seed.aliases.tsv"
    if aliases.is_file():
        subprocess.run([sys.executable, str(tool), "rename", str(aliases), str(manifest), str(title)], check=True)
    clone(manifest, game / "build/pso_manifest.tsv")
    os.symlink("../Resources/game/build/pso_manifest.tsv", macos / "pso_manifest.tsv")
    if any(line and not line.startswith("#") for line in title.read_text().splitlines()):
        clone(title, game / "build/pso_title.tsv")
        os.symlink("../Resources/game/build/pso_title.tsv", macos / "pso_title.tsv")


def main():
    native = BUILD / "build/odyssey-native"
    for need in (native, ICON, BUILD / "build/shader-service/totk_xlate", BUILD / "build/spirv-cross/spirv-cross",
                 BUILD / "extracted/local/romfs/romfs.bin", BUILD / "extracted/firmware/shared-fonts"):
        if not need.exists():
            raise SystemExit(f"missing {need}; build the Odyssey Native target and prepare the local data first")
    if APP.exists():
        shutil.rmtree(APP)
    APP.parent.mkdir(parents=True, exist_ok=True)
    contents = APP / "Contents"; macos = contents / "MacOS"; resources = contents / "Resources"; game = resources / "game"
    macos.mkdir(parents=True)
    clone(native, game / "build/odyssey-native")
    clone(BUILD / "build/shader-service/totk_xlate", game / "build/shader-service/totk_xlate")
    clone(BUILD / "build/spirv-cross/spirv-cross", game / "build/spirv-cross/spirv-cross")
    rewritten = BUILD / "rewritten"
    for item in rewritten.iterdir():
        if item.is_file() and not item.is_symlink():
            clone(item, game / "rewritten" / item.name)
    os.symlink("../rewritten", game / "rewritten/local")   # the module directory the runtime is given, as in Odyssey_build
    for name in ("romfs.bin", "runtime-index.bin"):
        clone(BUILD / "extracted/local/romfs" / name, game / "romfs" / name)
    # key 3's bigger render size (build_odyssey.py --render-res): the scaled graphics table and render-res.txt, which the
    # launcher hands the runtime as TOTK_ROMFS_PATCHES
    for item in sorted((BUILD / "extracted/local/romfs-patches").rglob("*")):
        if item.is_file():
            clone(item, game / "romfs-patches" / item.relative_to(BUILD / "extracted/local/romfs-patches"))
    for font in sorted((BUILD / "extracted/firmware/shared-fonts").glob("*.bfttf")):
        clone(font, game / "fonts" / font.name)
    slot = BUILD / "analysis/game-slot"
    if slot.is_dir():
        (game / "slot-dir.txt").write_text(str(slot) + "\n")
    os.symlink("../Resources/game/build/odyssey-native", macos / "odyssey-native")
    # Metal libraries precompiled from the shader cache (scripts/precompile_cached_shaders.py, needs Xcode's Metal
    # Toolchain). The runtime looks for them next to the executable path it was started by (nvn_mtl.m exe_rel, not
    # resolved), which here is Contents/MacOS, so they are linked there too.
    libs = BUILD / "build/shader-libs"
    if libs.is_dir():
        for lib in libs.glob("*.metallib"):
            clone(lib, game / "build/shader-libs" / lib.name)
        os.symlink("../Resources/game/build/shader-libs", macos / "shader-libs")
    # The whole-game shader seed (scripts/prepare_odyssey_shader_seed.py): the runtime imports build/shader-seed beside its
    # real executable (totk_shader_rt.m root_dir, symlinks resolved) into "Odyssey Data"'s cache on the first launch,
    # checking every file against seed-manifest.json; later launches only compare the receipt.
    seed = BUILD / "build/shader-seed"
    if (seed / "seed-manifest.json").is_file():
        subprocess.run(["cp", "-cR", str(seed), str(game / "build/shader-seed")], check=True)
    # Export Odyssey's shared pipeline recipes, as the TOTK/BOTW packager does. The shader seed carries libraries,
    # not render pipelines: without these rows the first scene compiled hundreds of pipelines on the submit thread.
    # Pipelines the title screen needs (Applier session, recorded 22 October): the first frame waits only for this list
    # (nvn_mtl.m warm_start reads it beside the shader map, i.e. the executable path, Contents/MacOS).
    package_warm_lists(macos, game)
    # The launcher: no window of its own, every setting is in the game's settings panel (key 1).
    subprocess.run(["xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(macos / "Odyssey"),
                    str(GAME / "scripts/odyssey_app_launcher.c")], check=True)
    subprocess.run(["sips", "-s", "format", "icns", str(ICON), "--out", str(resources / "OdysseyIcon.icns")],
                   check=True, stdout=subprocess.DEVNULL)
    version = ICON.with_name("game-version.txt")   # extract_app_icon.py: the update's version, for the window title
    if version.is_file():
        shutil.copyfile(version, resources / "game-version.txt")
    revision = subprocess.run(["git", "-C", str(GAME), "rev-parse", "--short=10", "HEAD"],
                              capture_output=True, text=True)
    if revision.returncode == 0:
        dirty = subprocess.run(["git", "-C", str(GAME), "status", "--porcelain", "--", "."],
                               capture_output=True, text=True)
        (resources / "source-commit.txt").write_text(revision.stdout.strip() + ("+local" if dirty.stdout else "") + "\n")
    info = {
        "CFBundleDevelopmentRegion": "en", "CFBundleDisplayName": "Odyssey", "CFBundleName": "Odyssey",
        "CFBundleExecutable": "Odyssey", "CFBundleIconFile": "OdysseyIcon",
        "CFBundleIdentifier": BUNDLE_ID, "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.2", "CFBundleVersion": "2",
        "LSApplicationCategoryType": "public.app-category.games", "LSMinimumSystemVersion": "26.0",
        "NSHighResolutionCapable": True,
    }
    if os.environ.get("ODYSSEY_RECORD_WORK"):   # what made the app, for MNG Launcher's update (build_app.record_update_source)
        info.update(MNGWorkFolder=os.environ["ODYSSEY_RECORD_WORK"], MNGSourceRepo=os.environ.get("ODYSSEY_RECORD_REPO", ""),
                    MNGBuildPython=os.environ.get("ODYSSEY_RECORD_PYTHON", sys.executable))
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump(info, file)
    # Ad hoc, with the bundle ID as the designated requirement (build_app.sign_app): macOS keeps the player's answer to
    # "files on a removable volume" by that requirement; the ad hoc default (the code hash) asked again after every build.
    subprocess.run(["codesign", "--force", "--sign", "-", f'-r=designated => identifier "{BUNDLE_ID}"', str(APP)],
                   check=True, stdout=subprocess.DEVNULL)
    seed_data()
    print(APP)


if __name__ == "__main__":
    main()
