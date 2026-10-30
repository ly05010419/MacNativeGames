#!/usr/bin/env python3
"""Package Animal Crossing: New Horizons into a self-contained macOS application bundle.

The bundle carries the native runtime, shader tools, rewritten modules, RomFS,
shared fonts, precompiled Metal shader libraries, the shader seed (when prepared), and launcher.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
GAME = HERE.parent
REPO_ROOT = GAME.parents[1]
WORKSPACE_ROOT = GAME.parents[2]
sys.path.insert(0, str(REPO_ROOT))
from games.acnh.prepare import prepared_profile
# The shader translator is carried in this folder (prebuilt/macos-arm64/README.md) and pinned by its .sha256.
TRANSLATOR = GAME / "prebuilt/macos-arm64/totk_xlate"
TRANSLATOR_SHA256 = GAME / "prebuilt/macos-arm64/totk_xlate.sha256"


def identity(path: Path):
    with path.open("rb") as f:
        return {"size": path.stat().st_size, "sha256": hashlib.file_digest(f, "sha256").hexdigest()}


def clone(source: Path, destination: Path):
    """APFS copy-on-write clone (no extra disk space); fallback to copy."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(["/bin/cp", "-c", str(source), str(destination)], capture_output=True)
    if result.returncode != 0:
        shutil.copy2(source, destination)


def verify_file(path: Path, expected_size: int, expected_sha256: str, name: str):
    if not path.is_file():
        raise ValueError(f"Missing required input: {path} ({name})")
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise ValueError(f"Size mismatch for {name}: expected {expected_size}, got {actual_size}")
    actual_sha256 = identity(path)["sha256"]
    if actual_sha256 != expected_sha256:
        raise ValueError(f"SHA-256 mismatch for {name}: expected {expected_sha256}, got {actual_sha256}")
    return {"size": actual_size, "sha256": actual_sha256}


def pinned_translator():
    """Check the prebuilt shader translator against its pinned SHA-256."""
    if not TRANSLATOR.is_file() or TRANSLATOR.is_symlink() or not os.access(TRANSLATOR, os.X_OK):
        raise ValueError(f"Missing or non-executable shader translator: {TRANSLATOR}")
    wanted = TRANSLATOR_SHA256.read_text().split()[0]
    translator = identity(TRANSLATOR)
    if translator["sha256"] != wanted:
        raise ValueError(f"Shader translator differs from the pinned one: {translator['sha256']} != {wanted}")
    return translator


def font_rows(header: Path):
    text = header.read_text()
    rows = re.findall(r'\{"([^"\n]+)",(\d+)ULL,(\d+)ULL,"([0-9a-f]{64})"\}', text)
    if len(rows) != 6 or "#define SHARED_FONT_COUNT 6" not in text:
        raise ValueError("Expected six generated shared-font rows in shared_fonts_config.h")
    result = {}
    for filename, size, payload, digest in rows:
        name = Path(filename).name
        if name in result or not name.endswith(".bfttf") or int(payload) + 8 != int(size):
            raise ValueError(f"Invalid generated shared-font row for {name}")
        result[name] = {"size": int(size), "sha256": digest}
    return result


def find_default_paths():
    work = WORKSPACE_ROOT / "ACNH_build/native-v1.2.1"
    host_info_path = work / "host-info.json"
    prepared = None
    fonts = None
    if host_info_path.is_file():
        try:
            info = json.loads(host_info_path.read_text())
            prepared = Path(info.get("prepared", ""))
            fonts = Path(info.get("shared_fonts", ""))
        except Exception:
            pass
    if not prepared or not prepared.exists():
        prepared = WORKSPACE_ROOT / "ACNH_build/verified-v458752"
    if not fonts or not fonts.exists():
        fonts = WORKSPACE_ROOT / "ACNH_build/shared-fonts"
    return work, prepared, fonts


def verify_inputs(work: Path, prepared: Path, fonts: Path):
    expected = prepared_profile(prepared)
    host = json.loads((work / "host-info.json").read_text())
    if host.get("display_version") != expected["display_version"] or host.get("prepared") != str(prepared.resolve()):
        raise ValueError("Host and prepared input versions/paths differ")
    verified = {}

    # 1. Native host executable
    runtime = work / "build/acnh-native"
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        raise ValueError(f"Missing or non-executable host: {runtime}")
    runtime_id = identity(runtime)
    verified["build/acnh-native"] = runtime_id
    runtime_bytes = runtime.read_bytes()
    marker = ("MNG_ACNH_VERSION:" + expected["display_version"]).encode()
    if marker not in runtime_bytes:
        raise ValueError("Runtime was built for another ACNH version")

    # 2. Shader translation tools
    verified["prebuilt/macos-arm64/totk_xlate"] = pinned_translator()

    # Optional: the translator has SPIRV-Cross compiled in and starts this program only with TOTK_XLATE_SPIRV_CROSS=process.
    spirv = work / "build/spirv-cross/spirv-cross"
    if spirv.exists():
        if not spirv.is_file() or not os.access(spirv, os.X_OK):
            raise ValueError(f"Non-executable spirv-cross: {spirv}")
        verified["build/spirv-cross/spirv-cross"] = identity(spirv)

    # 3. Rewritten modules
    rewritten_dir = work / "rewritten"
    module_shas = expected.get("rewritten_sha256")
    if module_shas is None:
        reference = json.loads((GAME / "assets" / f'preparation-v{expected["cnmt_version_raw"]}.json').read_text())
        module_shas = {name: row["patched_sha256"] for name, row in reference["rewrite"]["modules"].items()
                       if name != "rtld"}
    for name, sha in module_shas.items():
        module_path = rewritten_dir / f"{name}.patched.bin"
        if not module_path.is_file():
            raise ValueError(f"Missing rewritten module: {module_path}")
        mod_id = identity(module_path)
        if mod_id["sha256"] != sha:
            raise ValueError(f"Module {name} hash mismatch: {mod_id['sha256']} != {sha}")
        if sha.encode() not in runtime_bytes:
            raise ValueError(f"Runtime was not built with verified {name} identity")
        verified[f"rewritten/{name}.patched.bin"] = mod_id

    # 4. RomFS and index
    romfs_bin = prepared / "update/romfs/romfs.bin"
    romfs_idx = prepared / "update/romfs/runtime-index.bin"
    verified["romfs/romfs.bin"] = verify_file(
        romfs_bin, expected["romfs_size"], expected["romfs_sha256"], "romfs.bin"
    )
    if not romfs_idx.is_file():
        raise ValueError(f"Missing RomFS index: {romfs_idx}")
    verified["romfs/runtime-index.bin"] = identity(romfs_idx)

    # 5. Shared fonts
    header = work / "build/shared_fonts_config.h"
    if not header.is_file():
        raise ValueError(f"Missing font config header: {header}")
    rows = font_rows(header)
    for font_name, entry in rows.items():
        font_path = fonts / font_name
        verified[f"fonts/{font_name}"] = verify_file(
            font_path, entry["size"], entry["sha256"], font_name
        )

    # 6. Precompiled shader libraries
    shader_libs = work / "build/shader-libs"
    if shader_libs.is_dir():
        count = 0
        for lib in shader_libs.glob("*.metallib"):
            if lib.stat().st_size > 4:
                count += 1
        verified["shader_libs_count"] = count

    # 7. The shader seed (tools/prepare_shader_seed.py), when prepared: the game's own shaders, translated, for the
    # runtime to import and warm before play. It must come from this RomFS and this runtime's translation chain, and every
    # file must be the one its manifest names (the runtime checks them again at its first start).
    seed = work / "build/shader-seed"
    if (seed / "seed-manifest.json").is_file():
        manifest = json.loads((seed / "seed-manifest.json").read_text())
        chain = re.search(r'#define DMSH_CHAIN "([^"]+)"', (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text()).group(1)
        romfs_sha256 = json.loads((prepared / "update/romfs/index.json").read_text())["romfs_sha256"]
        if manifest.get("chain") != chain or manifest.get("source_romfs_sha256") != romfs_sha256 or not manifest.get("files"):
            raise ValueError(f"Shader seed {seed} was prepared for other inputs; prepare it again")
        for name, digest in manifest["files"].items():
            if "/" in name or identity(seed / name)["sha256"] != digest:
                raise ValueError(f"Shader seed file {name} does not match its manifest")
        verified["shader_seed"] = {"shaders": manifest["shaders"], "files": len(manifest["files"]),
                                   "manifest_sha256": identity(seed / "seed-manifest.json")["sha256"]}
    else:
        print(f"No shader seed in {seed}: the game translates shaders while playing (tools/prepare_shader_seed.py)")

    return expected, verified, rows


def seed_data(data_dir: Path, user_data_src: Path = None, cache_src: Path = None):
    data_dir.mkdir(parents=True, exist_ok=True)
    profile = data_dir / "profile"
    profile.mkdir(mode=0o700, exist_ok=True)
    (data_dir / "shader-cache").mkdir(parents=True, exist_ok=True)

    # If existing save data is found, seed it
    if user_data_src and user_data_src.is_dir():
        save_src = user_data_src / "save"
        if save_src.is_dir() and not (profile / "save").is_dir():
            shutil.copytree(save_src, profile / "save", dirs_exist_ok=True)
            print(f"Seeded save data from {user_data_src}")
        for item in user_data_src.glob("account.*"):
            if item.is_file() and not (profile / item.name).is_file():
                shutil.copy2(item, profile / item.name)

    # If existing shader cache is available, seed it
    if cache_src and cache_src.is_dir():
        dst_cache = data_dir / "shader-cache"
        for item in cache_src.glob("*.variants"):
            if not (dst_cache / item.name).exists():
                clone(item, dst_cache / item.name)


def package(work: Path, prepared: Path, fonts: Path, output: Path, icon: Path = None):
    if output.suffix != ".app":
        raise ValueError("--output must end with .app")
    if output.exists():
        print(f"Removing existing output bundle: {output}")
        shutil.rmtree(output)
    if not output.parent.is_dir():
        output.parent.mkdir(parents=True, exist_ok=True)

    expected, verified, font_info = verify_inputs(work, prepared, fonts)
    output.mkdir()

    try:
        contents = output / "Contents"
        macos = contents / "MacOS"
        resources = contents / "Resources"
        game = resources / "game"
        macos.mkdir(parents=True)
        game.mkdir(parents=True)

        # 1. Compile native launcher
        launcher_src = HERE / "acnh_app_launcher.c"
        launcher_bin = macos / "Animal Crossing"
        subprocess.run(
            ["xcrun", "clang", "-O2", "-Wall", "-Wextra", str(launcher_src), "-o", str(launcher_bin)],
            check=True,
        )

        # 2. Bundle game runtime and tools
        clone(work / "build/acnh-native", game / "build/acnh-native")
        clone(TRANSLATOR, game / "build/shader-service/totk_xlate")   # where totk_shader_rt.m looks for it
        if "build/spirv-cross/spirv-cross" in verified:
            clone(work / "build/spirv-cross/spirv-cross", game / "build/spirv-cross/spirv-cross")

        # Symlink runtime in MacOS
        os.symlink("../Resources/game/build/acnh-native", macos / "acnh-native")

        # 3. Bundle rewritten modules
        for mod_name in ("main", "subsdk0", "sdk"):
            mod_file = work / f"rewritten/{mod_name}.patched.bin"
            clone(mod_file, game / f"rewritten/{mod_name}.patched.bin")

        # 4. Bundle RomFS
        clone(prepared / "update/romfs/romfs.bin", game / "romfs/romfs.bin")
        clone(prepared / "update/romfs/runtime-index.bin", game / "romfs/runtime-index.bin")

        # 5. Bundle shared fonts
        for font_name in font_info:
            clone(fonts / font_name, game / f"fonts/{font_name}")

        # 6. Bundle precompiled shader libraries
        libs_dir = work / "build/shader-libs"
        if libs_dir.is_dir():
            target_libs = game / "build/shader-libs"
            target_libs.mkdir(parents=True, exist_ok=True)
            for lib in libs_dir.glob("*.metallib"):
                with lib.open("rb") as f:
                    if f.read(4) != b"MTLB":
                        continue
                clone(lib, target_libs / lib.name)
            compiler_txt = libs_dir / "compiler.txt"
            if compiler_txt.is_file():
                clone(compiler_txt, target_libs / "compiler.txt")
            os.symlink("../Resources/game/build/shader-libs", macos / "shader-libs")

        # 6b. Bundle the shader seed: imported from Resources/game/build/shader-seed (totk_shader_rt.m root_dir), its
        # warm-up list read from Contents/MacOS/shader-seed (nvn_mtl.m warm_start, beside shader-map.tsv).
        if "shader_seed" in verified:
            seed = work / "build/shader-seed"
            (game / "build").mkdir(parents=True, exist_ok=True)
            subprocess.run(["/bin/cp", "-cR", str(seed), str(game / "build/shader-seed")], check=True)   # one APFS clone of the folder
            os.symlink("../Resources/game/build/shader-seed", macos / "shader-seed")
            print(f"Bundled the shader seed: {verified['shader_seed']['shaders']} shaders, {verified['shader_seed']['files']} files")

        # 7. Bundle licenses
        notices = resources / "Licenses"
        notices.mkdir(parents=True, exist_ok=True)
        for path in sorted((GAME / "third_party").rglob("*")):
            if path.is_file() and (path.name.lower().startswith("license") or "license" in path.name.lower()):
                dest = notices / path.relative_to(GAME / "third_party")
                dest.parent.mkdir(parents=True, exist_ok=True)
                clone(path, dest)
        if (REPO_ROOT / "LICENSE").is_file():
            clone(REPO_ROOT / "LICENSE", notices / "MacNativeGames-MIT.txt")

        # 8. Bundle icon
        icon_file_name = "AnimalCrossingIcon"
        if icon and icon.is_file():
            clone(icon, resources / f"{icon_file_name}.icns")
        elif (work / "build/AppIcon.icns").is_file():
            clone(work / "build/AppIcon.icns", resources / f"{icon_file_name}.icns")

        # 9. Write Info.plist
        info = {
            "CFBundleDevelopmentRegion": "en",
            "CFBundleDisplayName": "Animal Crossing: New Horizons",
            "CFBundleExecutable": "Animal Crossing",
            "CFBundleIconFile": icon_file_name,
            "CFBundleIdentifier": "com.nintendo.acnh.native",
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": "Animal Crossing",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": expected["display_version"],
            "CFBundleVersion": "1",
            "LSApplicationCategoryType": "public.app-category.games",
            "LSMinimumSystemVersion": os.environ.get("MACOSX_DEPLOYMENT_TARGET", "26.0"),
            "NSHighResolutionCapable": True,
        }
        # The source revision, when the project is a Git checkout (a downloaded archive packages without it).
        commit = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=False)
        if commit.returncode == 0:
            dirty = subprocess.run(["git", "-C", str(REPO_ROOT), "status", "--porcelain", "--", "games/acnh"],
                                   capture_output=True, text=True, check=False).stdout.strip()
            info["MNGSourceCommit"] = commit.stdout.strip()
            info["MNGSourceRevision"] = commit.stdout.strip() + ("-dirty" if dirty else "")
        with (contents / "Info.plist").open("wb") as f:
            plistlib.dump(info, f)

        # 10. Write build receipt
        receipt = {
            "game": "Animal Crossing: New Horizons",
            "version": expected["display_version"],
            "support_level": expected.get("support_level", "verified"),
            "title_id": expected.get("title_id", "01006F8002326000"),
            "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "inputs": verified,
            "packager": identity(Path(__file__)),
            "launcher_source": identity(launcher_src),
        }
        (resources / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")

        # 11. Codesign the bundle
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(output)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(output)], check=True)

        # 12. Run launcher self-check
        subprocess.run([str(launcher_bin), "--check"], check=True)

        # 13. Seed application data directory next to the app
        app_data_dir = output.parent / "Animal Crossing Data"
        # Newer versions start with an isolated profile. Save migration requires
        # its own compatibility check; never seed 3.x from an unrelated old save.
        user_data_local = WORKSPACE_ROOT / "user-data" if expected["cnmt_version_raw"] == 458752 else None
        cache_local = work / "build/shader-cache/rt-totk-native-v4-unverified"
        seed_data(app_data_dir, user_data_local, cache_local)

        # Finder and the Dock keep the icon they cached for this path (reported in play, PR #1305: the app showed the old, blank
        # icon in Finder while the running app's was right): a new modification date and Launch Services registration
        # make them read the new bundle's.
        os.utime(output)
        lsregister = Path("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister")
        if lsregister.is_file():
            subprocess.run([str(lsregister), "-f", str(output)], check=False)
        print(f"Successfully packaged and verified: {output}")

    except BaseException:
        if output.exists():
            shutil.rmtree(output)
        raise


def main():
    default_work, default_prepared, default_fonts = find_default_paths()
    default_output = WORKSPACE_ROOT / "apps/Animal Crossing New Horizons.app"
    default_icon = default_work / "build/AppIcon.icns"

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=default_work, help="Path to native work directory")
    parser.add_argument("--prepared", type=Path, default=default_prepared, help="Path to prepared verified directory")
    parser.add_argument("--shared-fonts", type=Path, default=default_fonts, help="Path to shared fonts directory")
    parser.add_argument("--output", type=Path, default=default_output, help="Path for output .app bundle")
    parser.add_argument("--icon", type=Path, default=default_icon if default_icon.is_file() else None, help="Path to .icns file")
    parser.add_argument("--verify-only", action="store_true", help="Verify inputs without packaging")

    args = parser.parse_args()
    work = args.work.resolve()
    prepared = args.prepared.resolve()
    fonts = args.shared_fonts.resolve()
    output = args.output.resolve()
    icon = args.icon.resolve() if args.icon else None

    try:
        if args.verify_only:
            expected, _, _ = verify_inputs(work, prepared, fonts)
            print(f'All required inputs match Animal Crossing: New Horizons {expected["display_version"]}.')
        else:
            package(work, prepared, fonts, output, icon)
    except (OSError, ValueError, subprocess.CalledProcessError) as err:
        parser.exit(1, f"Packaging failed: {err}\n")


if __name__ == "__main__":
    main()
