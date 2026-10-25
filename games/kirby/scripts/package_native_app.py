#!/usr/bin/env python3
"""Package an already prepared, verified Kirby 1.0.0 work directory.

This is not an NSP extraction or rewriting tool. It accepts only the exact
prepared inputs listed in assets/expected-v1.0.0.json, and never replaces an app.
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

HERE = Path(__file__).resolve().parent
GAME = HERE.parent
EXPECTED = GAME / "assets/expected-v1.0.0.json"


def identity(path):
    with path.open("rb") as f:
        return {"size": path.stat().st_size, "sha256": hashlib.file_digest(f, "sha256").hexdigest()}


def verify_file(path, expected):
    if not path.is_file():
        raise ValueError(f"Missing required input: {path}")
    if path.stat().st_size != expected["size"]:
        raise ValueError(f"Unsupported or damaged build: size mismatch for {path.name}")
    actual = identity(path)
    if actual["sha256"] != expected["sha256"]:
        raise ValueError(f"Unsupported or damaged build: SHA-256 mismatch for {path.name}")
    if "build_id" in expected:
        with path.open("rb") as f:
            header = f.read(0x60)
        if header[:4] != b"NSO0" or header[0x40:0x60].hex() != expected["build_id"]:
            raise ValueError(f"Unsupported NSO build ID: {path.name}")
    return actual


def font_rows(header):
    text = header.read_text()
    rows = re.findall(r'\{"([^"\n]+)",(\d+)ULL,(\d+)ULL,"([0-9a-f]{64})"\}', text)
    if len(rows) != 6 or "#define SHARED_FONT_COUNT 6" not in text:
        raise ValueError("Expected six generated shared-font rows")
    result = {}
    for filename, size, payload, digest in rows:
        name = Path(filename).name
        if name in result or not name.endswith(".bfttf") or int(payload) + 8 != int(size):
            raise ValueError("Invalid generated shared-font row")
        result[name] = {"size": int(size), "sha256": digest}
    return result


def verify_inputs(work, data_work, fonts):
    expected = json.loads(EXPECTED.read_text())
    verified = {}
    for rel, entry in expected["files"].items():
        source = (work if rel.startswith("build/") else data_work) / rel
        verified[rel] = verify_file(source, entry)
    runtime = work / "build/kirby-native"
    runtime_bytes = runtime.read_bytes()
    # These identities are compiled into runtime_tables.h. The loader also checks
    # each module before making executable pages available.
    for name in ("main", "sdk", "subsdk0"):
        digest = expected["files"][f"rewritten/{name}.patched.bin"]["sha256"]
        if digest.encode() not in runtime_bytes:
            raise ValueError(f"Runtime was not built with the supported {name} identity")
    verified["build/kirby-native"] = identity(runtime)
    rows = font_rows(work / "build/shared_fonts_config.h")
    for name, entry in rows.items():
        verified["fonts/" + name] = verify_file(fonts / name, entry)
        if entry["sha256"].encode() not in runtime_bytes:
            raise ValueError(f"Runtime font metadata does not match: {name}")
    for rel in ("romfs/romfs.bin", "romfs/runtime-index.bin"):
        if expected["files"][rel]["sha256"].encode() not in runtime_bytes:
            raise ValueError(f"Runtime RomFS metadata does not match: {rel}")
    verified["build/shared_fonts_config.h"] = identity(work / "build/shared_fonts_config.h")
    translator = work / "build/shader-service/totk_xlate"
    translator_identity = identity(translator)
    wanted = (GAME / "prebuilt/macos-arm64/totk_xlate.sha256").read_text().split()[0]
    if translator_identity["sha256"] != wanted:
        raise ValueError("Shader translator differs from the pinned Kirby translator")
    verified["build/shader-service/totk_xlate"] = translator_identity
    verified["build/spirv-cross/spirv-cross"] = identity(work / "build/spirv-cross/spirv-cross")
    for rel in ("build/kirby-native", "build/shader-service/totk_xlate", "build/spirv-cross/spirv-cross"):
        if not os.access(work / rel, os.X_OK):
            raise ValueError(f"Input is not executable: {rel}")
    return expected, verified, rows


def clone(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(["/bin/cp", "-c", str(source), str(destination)], capture_output=True)
    if result.returncode:
        shutil.copy2(source, destination)


def require_new_output(output):
    if output.suffix != ".app":
        raise ValueError("--output must name a new .app directory")
    if os.path.lexists(output):
        raise ValueError(f"Refusing to replace existing output: {output}; choose a new name")
    if not output.parent.is_dir():
        raise ValueError("The output parent directory must already exist")


def package(work, data_work, fonts, output, icon=None):
    require_new_output(output)
    expected, verified, rows = verify_inputs(work, data_work, fonts)
    if icon is not None and not icon.is_file():
        raise ValueError(f"Missing icon: {icon}")
    # mkdir is exclusive: an existing app, file or symlink cannot be overwritten.
    output.mkdir()
    try:
        contents = output / "Contents"
        macos = contents / "MacOS"
        resources = contents / "Resources"
        game = resources / "game"
        macos.mkdir(parents=True)
        game.mkdir(parents=True)
        subprocess.run(["xcrun", "clang", "-O2", "-Wall", "-Wextra", str(HERE / "kirby_app_launcher.c"),
                        "-o", str(macos / "Kirby")], check=True)
        for rel in verified:
            if rel.startswith("extracted/") or rel.endswith(".h"):
                continue
            source = fonts / Path(rel).name if rel.startswith("fonts/") else (
                work if rel.startswith("build/") else data_work) / rel
            clone(source, game / rel)
        os.symlink("../Resources/game/build/kirby-native", macos / "kirby-native")
        libraries = work / "build/shader-libs"
        if libraries.is_dir():
            for lib in sorted(libraries.glob("*.metallib")):
                with lib.open("rb") as f:
                    if f.read(4) != b"MTLB":
                        raise ValueError(f"Invalid Metal library: {lib.name}")
                clone(lib, game / "build/shader-libs" / lib.name)
            if (libraries / "compiler.txt").is_file():
                clone(libraries / "compiler.txt", game / "build/shader-libs/compiler.txt")
        notices = resources / "Licenses"
        for path in sorted((GAME / "third_party").rglob("*")):
            if path.is_file() and (path.name.lower().startswith("license") or path.parent.name == "licenses"):
                clone(path, notices / path.relative_to(GAME / "third_party"))
        clone(GAME.parents[1] / "LICENSE", notices / "MacNativeGames-MIT.txt")
        info = {
            "CFBundleDevelopmentRegion": "en", "CFBundleDisplayName": expected["game"],
            "CFBundleExecutable": "Kirby", "CFBundleIdentifier": "local.projectidea.kirby",
            "CFBundleInfoDictionaryVersion": "6.0", "CFBundleName": "Kirby",
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0.0",
            "CFBundleVersion": "1", "LSApplicationCategoryType": "public.app-category.games",
            "LSMinimumSystemVersion": "26.0", "NSHighResolutionCapable": True,
        }
        if icon:
            clone(icon, resources / "KirbyIcon.icns")
            info["CFBundleIconFile"] = "KirbyIcon"
        with (contents / "Info.plist").open("wb") as f:
            plistlib.dump(info, f)
        receipt = {"game": expected["game"], "version": expected["version"], "title_id": expected["title_id"],
                   "reference_commit": expected["reference_commit"], "inputs": verified,
                   "packager": identity(Path(__file__)), "launcher_source": identity(HERE / "kirby_app_launcher.c")}
        (resources / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(output)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(output)], check=True)
        subprocess.run([str(macos / "Kirby"), "--check"], check=True)
    except BaseException:
        # Only the directory exclusively created by this invocation is removed.
        shutil.rmtree(output)
        raise
    print(f"Packaged and signature-checked: {output}")
    print("Launcher --check passed; this does not establish gameplay correctness.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True, help="prepared build directory containing build/")
    parser.add_argument("--data-work", type=Path, help="prepared extracted/, rewritten/ and romfs/ directory; default: --work")
    parser.add_argument("--shared-fonts", type=Path, required=True, help="your six BFTTF files matching the generated runtime header")
    parser.add_argument("--output", type=Path, required=True, help="new .app path; existing outputs are refused")
    parser.add_argument("--icon", type=Path, help="optional local .icns file")
    parser.add_argument("--verify-only", action="store_true", help="verify prepared inputs without creating or launching an app")
    args = parser.parse_args()
    work = args.work.resolve()
    data_work = (args.data_work or args.work).resolve()
    try:
        if args.verify_only:
            verify_inputs(work, data_work, args.shared_fonts.resolve())
            print("Prepared inputs match Kirby and the Forgotten Land 1.0.0.")
        else:
            package(work, data_work, args.shared_fonts.resolve(), args.output.absolute(), args.icon)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Kirby packaging failed: {error}\n")


if __name__ == "__main__":
    main()
