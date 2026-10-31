#!/usr/bin/env python3
"""Package a local native Tomodachi app; game data stays in the external host folder."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from games.tomodachi.tools.common import outside_repository, verified_file
from games.tomodachi.tools.romfs_index import index_romfs


def package_icon(control, resources):
    """Use the official local Control RomFS artwork, without re-extracting the game."""
    entries = {entry["path"]: entry for entry in index_romfs(control)}
    languages = ("AmericanEnglish", "BritishEnglish", "Japanese", "TraditionalChinese", "SimplifiedChinese")
    icon = next((entries[f"icon_{language}.dat"] for language in languages
                 if f"icon_{language}.dat" in entries), None)
    if icon is None:
        raise ValueError("Official icon missing from prepared control data")
    with tempfile.TemporaryDirectory(prefix="app-icon-") as temporary:
        folder = Path(temporary)
        source = folder / "official.jpg"
        with control.open("rb") as stream:
            stream.seek(icon["offset"])
            source.write_bytes(stream.read(icon["size"]))
        subprocess.run(["sips", "-s", "format", "png", str(source), "--out", str(resources / "AppIcon.png")],
                       check=True, capture_output=True)
        iconset = folder / "AppIcon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-s", "format", "png", "-z", str(size * scale), str(size * scale),
                                str(source), "--out", str(iconset / name)], check=True, capture_output=True)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(resources / "AppIcon.icns")], check=True)


def package(work, output, replace=False, language=None, backend="metal3"):
    if backend not in ("metal3", "metal4"): raise ValueError("Unknown renderer backend")
    binary_name = "tomodachi-native-metal4" if backend == "metal4" else "tomodachi-native"
    work, output = outside_repository(work,ROOT), outside_repository(output,ROOT)
    if output.suffix != ".app":
        raise ValueError("Output must end in .app")
    info = json.loads((work / "host-info.json").read_text())
    if info["title_id"] != "010051F0207B2000":
        raise ValueError("Wrong game host")
    verified_file(work / "build" / binary_name)
    verified_file(work / "build/spirv-cross/spirv-cross",info["spirv_cross_sha256"])
    control = Path(info["prepared"]) / "control/romfs.bin"
    verified_file(control)
    if output.exists():
        previous_language=output / "Contents/Resources/language.txt"
        if language is None and previous_language.exists(): language=previous_language.read_text().strip()
        if not replace or output.is_symlink() or not output.is_dir():
            raise ValueError("App already exists; use --replace to archive the previous build")
    installed = output
    installed.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="tomodachi-package-", dir=output.parent))
    output = stage / installed.name
    try:
        _assemble(work, output, info, control, binary_name, language, backend, installed)
        backup = None
        if installed.exists():
            (work / "tests").mkdir(exist_ok=True)
            backup = Path(tempfile.mkdtemp(prefix="app-", dir=work / "tests")) / (installed.name + ".backup")
            shutil.move(installed, backup)
        try:
            output.rename(installed)
        except OSError:
            if backup is not None:
                shutil.move(backup, installed)
            raise
    finally:
        shutil.rmtree(stage)
    print(installed)


def _assemble(work, output, info, control, binary_name, language, backend, installed):
    """Complete and verify a new package before moving the installed app."""
    macos, resources = output / "Contents/MacOS", output / "Contents/Resources"
    macos.mkdir(parents=True)
    resources.mkdir()
    package_icon(control, resources)
    shutil.copy2(work / "build" / binary_name,macos / "Tomodachi")
    (resources / "host-path.txt").write_text(os.path.relpath(work,installed / "Contents/Resources") + "\n")
    (resources / "romfs-path.txt").write_text(os.path.relpath(Path(info["prepared"]) / "romfs",installed / "Contents/Resources") + "\n")
    if language:
        (resources / "language.txt").write_text(language + "\n")
    metadata = dict(CFBundleExecutable="Tomodachi",CFBundleIdentifier="org.macnativegames.tomodachi",
                    CFBundleName="Tomodachi Life",CFBundleDisplayName="Tomodachi Life",
                    CFBundleIconFile="AppIcon",LSApplicationCategoryType="public.app-category.simulation-games",
                    GCSupportsControllerUserInteraction=True,
                    CFBundlePackageType="APPL",CFBundleShortVersionString="1.0.0",CFBundleVersion="1",
                    LSMinimumSystemVersion="26.0" if backend == "metal4" else "15.0",
                    MNGRendererBackend=backend,NSHighResolutionCapable=True,LSSupportsGameMode=True,
                    NSBluetoothAlwaysUsageDescription="Connect a wireless game controller.",
                    NSBluetoothPeripheralUsageDescription="Connect a wireless game controller.")
    with (output / "Contents/Info.plist").open("wb") as stream:
        revision = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=7", "HEAD"],
                                  capture_output=True, text=True)
        if revision.returncode == 0:
            metadata["MNGSourceCommit"] = revision.stdout.strip()
        plistlib.dump(metadata,stream)
    subprocess.run(["codesign","--force","--sign","-",str(output)],check=True)
    subprocess.run(["codesign","--verify","--strict",str(output)],check=True)


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("metal3", "metal4"), default="metal3")
    parser.add_argument("--work",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--language",choices=("ja","en-US","en-GB","fr","fr-CA","de","es","es-419","it","nl","pt","pt-BR","ru","ko","zh-Hans","zh-Hant"))
    parser.add_argument("--replace",action="store_true",help="Archive the previous app before packaging")
    args=parser.parse_args()
    try:package(args.work,args.output,args.replace,args.language,args.backend)
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as error:
        parser.exit(2,f"Tomodachi packaging failed: {error}\n")
