#!/usr/bin/env python3
"""Copy verified, user-owned TMNT data into a local iPad app before signing.

The output contains copyrighted game data. Never commit, upload, or distribute it.
"""
import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
MODULES = ("main", "subsdk0", "subsdk2", "subsdk1")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def expected(header, macro):
    match = re.search(rf'^#define {macro} "([0-9a-f]{{64}})"$', header.read_text(), re.M)
    if not match:
        raise ValueError(f"missing {macro} in private build header")
    return match.group(1)


def package(bundle, app, build_headers):
    bundle, app, build_headers = map(Path.resolve, (bundle, app, build_headers))
    if app == REPO or REPO in app.parents:
        raise ValueError("game data must stay outside the source checkout")
    if not (app / "Info.plist").is_file() or not (app / "TMNTiPad").is_file():
        raise ValueError("build the iPad app before packaging game data")
    target = app / "TMNT"
    if target.exists() or target.is_symlink():
        raise ValueError("app already has TMNT data; use a new clean app build")
    manifest = json.loads((bundle / "BUILD-MANIFEST.json").read_text())
    if not manifest.get("private"):
        raise ValueError("expected a verified private TMNT bundle")
    romfs = bundle / "romfs/romfs.bin"
    index = bundle / "romfs/runtime-index.bin"
    rh = build_headers / "romfs_config.h"
    if digest(romfs) != manifest["romfs_sha256"] or digest(romfs) != expected(rh, "ROMFS_IMAGE_SHA256"):
        raise ValueError("unsupported or modified RomFS")
    if digest(index) != manifest["index_sha256"] or digest(index) != expected(rh, "ROMFS_INDEX_SHA256"):
        raise ValueError("RomFS index does not match the linked runtime")
    nro = build_headers / "tmnt_nro_digest.h"
    nro_id = expected(nro, "TMNT_NRO_ID")
    source_nro = bundle / "rewritten/nro"
    for suffix, macro in (("fixups.tsv", "TMNT_NRO_LAYOUT"), ("gates.tsv", "TMNT_NRO_GATES")):
        if digest(source_nro / f"{nro_id}.{suffix}") != expected(nro, macro):
            raise ValueError("NRO rewrite metadata differs from the linked runtime")
    fonts = sorted((bundle / "shared-fonts").glob("*.bfttf"))
    if len(fonts) != 6 or not all(p.is_file() for p in fonts):
        raise ValueError("six shared fonts are required")
    font_config = (build_headers / "shared_fonts_config.h").read_text()
    for font in fonts:
        match = re.search(rf'\{{"shared-fonts/{re.escape(font.name)}",\d+ULL,\d+ULL,"([0-9a-f]{{64}})"\}}', font_config)
        if not match or digest(font) != match.group(1):
            raise ValueError(f"shared font does not match the linked runtime: {font.name}")
    for module in MODULES:
        for suffix in ("fixups.tsv", "gates.tsv"):
            if not (bundle / "rewritten" / f"{module}.{suffix}").is_file():
                raise ValueError(f"missing {module} rewrite metadata")
    # All checks precede creation. Copy actual bytes, never external-volume links.
    (target / "romfs").mkdir(parents=True)
    (target / "rewritten/nro").mkdir(parents=True)
    (target / "fonts").mkdir()
    shutil.copy2(romfs, target / "romfs/romfs.bin")
    shutil.copy2(index, target / "romfs/runtime-index.bin")
    for font in fonts:
        shutil.copy2(font, target / "fonts" / font.name)
    for module in MODULES:
        for suffix in ("fixups.tsv", "gates.tsv"):
            name = f"{module}.{suffix}"
            shutil.copy2(bundle / "rewritten" / name, target / "rewritten" / name)
    for suffix in ("fixups.tsv", "gates.tsv"):
        name = f"{nro_id}.{suffix}"
        shutil.copy2(source_nro / name, target / "rewritten/nro" / name)
    if (target / "romfs/romfs.bin").is_symlink() or digest(target / "romfs/romfs.bin") != manifest["romfs_sha256"]:
        raise ValueError("packaged RomFS verification failed")
    print(f"Private iPad resources packaged: {target}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--build-headers", type=Path, required=True)
    args = parser.parse_args()
    package(args.bundle, args.app, args.build_headers)
