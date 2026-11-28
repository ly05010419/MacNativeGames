#!/usr/bin/env python3
"""Package the prepared AoC 1.4.1 native build as a standalone macOS app.

The app contains the runtime, shader tools, rewritten modules and RomFS indexes.
The 11.7 GB RomFS image stays outside the bundle in:
~/Library/Application Support/MacNativeGames/AOCX/GameData/romfs.bin

An existing external image is never overwritten: it must match the expected
size and SHA-256. A missing image is installed using an APFS clone when possible.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
REPO = GAME.parents[1]

if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from tools.port_app import bundle  # noqa: E402

EXPECTED = json.loads((GAME / "assets/expected-1.4.1.json").read_text())

BUNDLE_ID = "org.macnativegames.aocx"
EXECUTABLE = "AOCX"
NAME = "Hyrule Warriors: Age of Calamity"
DISPLAY_NAME = NAME
VERSION = EXPECTED["version"]

ROMFS_FILES = ("index.json", "runtime-index.bin")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_file(path, size, digest, description):
    require(path.is_file(), f"{description} missing: {path}")
    require(path.stat().st_size == size, f"{description} has the wrong size: {path}")
    actual = sha256(path)
    require(actual == digest, f"{description} SHA-256 mismatch: {path}")
    return path


def verify(work):
    """Verify all files required by the app and the external game-data image."""
    files = {
        "runtime": work / "build/aocx-native",
        "translator": work / "build/shader-service/totk_xlate",
        "spirv-cross": work / "build/spirv-cross/spirv-cross",
    }

    for name, path in files.items():
        require(path.is_file() and os.access(path, os.X_OK),
                f"missing executable {name}: {path}")

    modules = work / "rewritten/local"
    for module, expected_hash in EXPECTED["rewritten_sha256"].items():
        path = modules / module
        require(path.is_file() and sha256(path) == expected_hash,
                f"unverified rewritten module: {path}")

    files["modules"] = modules

    romfs = work / "extracted/local/romfs"
    for name in ROMFS_FILES:
        require((romfs / name).is_file(), f"RomFS {name} missing: {romfs}")

    config = (work / "build/romfs_config.h").read_text()
    size_match = re.search(r"#define ROMFS_IMAGE_SIZE (\d+)ULL", config)
    index_match = re.search(r'#define ROMFS_INDEX_SHA256 "([0-9a-f]{64})"', config)

    require(size_match is not None, "ROMFS_IMAGE_SIZE missing from romfs_config.h")
    require(index_match is not None, "ROMFS_INDEX_SHA256 missing from romfs_config.h")

    image_size = int(size_match.group(1))
    image_hash = EXPECTED["romfs_sha256"]

    require(image_hash == "42192ad84676805dbc76f5cdd834b8d27c84b7849de6d7eebbdcce8f6e837f6c",
            "unexpected expected-1.4.1.json RomFS hash")

    image = romfs / "romfs.bin"
    verify_file(image, image_size, image_hash, "RomFS image")

    runtime_index = romfs / "runtime-index.bin"
    verify_file(runtime_index, runtime_index.stat().st_size,
                index_match.group(1), "RomFS runtime index")

    files["romfs"] = romfs
    files["romfs_image"] = image
    files["romfs_size"] = image_size
    files["romfs_hash"] = image_hash

    return files


def install_external_romfs(files):
    """Install only when absent; never overwrite an existing user game image."""
    destination_dir = (
        Path.home()
        / "Library/Application Support/MacNativeGames/AOCX/GameData"
    )
    destination = destination_dir / "romfs.bin"

    destination_dir.mkdir(parents=True, exist_ok=True)

    if destination.is_symlink():
        raise ValueError(f"refusing to use a symlink as external RomFS: {destination}")

    if destination.exists():
        verify_file(destination, files["romfs_size"], files["romfs_hash"],
                    "Existing external RomFS")
        print(f"External RomFS already verified: {destination}")
        return destination

    temporary = destination.with_name(
        destination.name + f".partial-{os.getpid()}"
    )
    require(not temporary.exists(), f"temporary file already exists: {temporary}")

    try:
        print(f"Installing external RomFS: {destination}")
        bundle.clone_file(files["romfs_image"], temporary)
        verify_file(temporary, files["romfs_size"], files["romfs_hash"],
                    "Installed external RomFS")
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            temporary.unlink()

    print(f"External RomFS installed: {destination}")
    return destination


def package(work, output, icon=None, record=None):
    work = work.resolve()
    output = output.resolve()

    require(output.suffix == ".app", "--output must end in .app")
    require(work not in output.parents and output not in work.parents,
            "the app and work folder must be separate")

    files = verify(work)
    install_external_romfs(files)

    if icon is None and (work / "build/AppIcon.icns").is_file():
        icon = work / "build/AppIcon.icns"

    with bundle.staged(output) as stage:
        assemble(stage, files, icon, record)

    bundle.register(output)
    print(f"Packaged: {output}")
    return output


def assemble(stage, files, icon, record):
    """Assemble, sign and self-check the app."""
    contents = stage / "Contents"
    macos = contents / "MacOS"
    game = contents / "Resources/game"

    macos.mkdir(parents=True)

    bundle.clone_file(files["runtime"], game / "build/aocx-native")
    bundle.clone_file(files["translator"],
                      game / "build/shader-service/totk_xlate")
    bundle.clone_file(files["spirv-cross"],
                      game / "build/spirv-cross/spirv-cross")

    bundle.clone_tree(files["modules"], game / "rewritten/local")

    for name in ROMFS_FILES:
        bundle.clone_file(files["romfs"] / name, game / "romfs" / name)

    (game / "fonts").mkdir(parents=True)

    bundle.compile_launcher(
        GAME / "app/launcher.c",
        macos / EXECUTABLE,
        frameworks=("CoreFoundation",),
    )

    icon_file = bundle.icon(icon, contents / "Resources", "AppIcon")

    info = bundle.bundle_info(
        executable=EXECUTABLE,
        bundle_id=BUNDLE_ID,
        name=NAME,
        display_name=DISPLAY_NAME,
        version=VERSION,
        build="1",
        minimum_system=bundle.deployment_target(),
        icon_file=icon_file,
    )

    commit = bundle.source_commit(REPO)
    bundle.write_info(
        stage,
        {
            **info,
            **({"MNGSourceCommit": commit} if commit else {}),
            **(record or {}),
        },
    )

    bundle.sign(stage, BUNDLE_ID, deep=True)
    bundle.verify_signature(stage)
    bundle.check_launcher(macos / EXECUTABLE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--icon", type=Path)
    parser.add_argument("--record-work", type=Path)
    args = parser.parse_args()

    record = (
        bundle.update_record(args.record_work.resolve(), REPO)
        if args.record_work else None
    )

    try:
        package(args.work, args.output, args.icon, record)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ERROR: packaging failed: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
