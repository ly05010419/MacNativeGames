#!/usr/bin/env python3
"""Package a local XCX diagnostic app; it requires its external work directory."""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess


def package(work, output, extended_va=False, gpu_submit=False):
    work, output = work.expanduser().resolve(), output.expanduser().absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; choose a new bundle path")
    if not (work / "build/xcx-native").is_file():
        raise ValueError("Build the XCX runtime first")
    if gpu_submit:
        for tool in ("shader-service/totk_xlate", "spirv-cross/spirv-cross"):
            path = work / "build" / tool
            if not path.is_file() or not os.access(path, os.X_OK):
                raise ValueError(f"Build shader-tools first: missing executable {path}")
    contents = output / "Contents"
    binary = contents / "MacOS"
    binary.mkdir(parents=True)
    source = Path(__file__).resolve().parents[1] / "app/launcher.c"
    subprocess.run(["xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(source), "-framework", "CoreFoundation", "-o", str(binary / "xcx-launcher")], check=True)
    shutil.copy2(work / "build/xcx-native", binary / "xcx-native")
    info = dict(CFBundleName="Xenoblade Chronicles X Experiment", CFBundlePackageType="APPL",
                CFBundleExecutable="xcx-launcher", CFBundleIdentifier="private.macnativegames.xcx.experiment",
                CFBundleShortVersionString="1.0.0", CFBundleVersion="1", NSHighResolutionCapable=True,
                XCXWorkFolder=str(work), XCXExtendedVA=extended_va, XCXGPUSubmit=gpu_submit)
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    subprocess.run(["codesign", "--force", "--sign", "-", "--deep", str(output)], check=True)
    subprocess.run(["codesign", "--verify", "--strict", "--deep", str(output)], check=True)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--extended-va", action="store_true", help="Opt into the experimental 40-bit macOS guest layout")
    parser.add_argument("--gpu-submit", action="store_true", help="Enable experimental GPU submissions; requires local shader-tools")
    args = parser.parse_args()
    print(package(args.work, args.output, args.extended_va, args.gpu_submit))
