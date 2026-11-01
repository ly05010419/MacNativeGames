#!/usr/bin/env python3
"""Verify and prepare Tomodachi 1.0.0 for native-host development.

Generated code, game data and reports belong in an external work directory.
No Ryujinx configuration or save data is read or changed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from games.tomodachi.tools.common import outside_repository, verified_file
from games.tomodachi.tools.identity import select_program

TITLE_ID = "010051F0207B2000"


def check_exefs(work):
    expected = json.loads((HERE / "assets/expected-v1.0.0.json").read_text())
    manifest = json.loads((work / "extraction-manifest.json").read_text())
    actual = {row["name"]: row["sha256"] for row in manifest["files"]}
    if len(actual) != len(manifest["files"]) or actual != expected["exefs_sha256"]:
        raise ValueError("Unknown executable build; only the pinned Tomodachi 1.0.0 is supported")
    for name, digest in actual.items():
        verified_file(work / "exefs" / name, digest)
    return expected


def prepare_control(source, keys_path, work, expected):
    from games.tomodachi.tools.extract_exefs import load_keys, container_entries, common_tickets, nca_inventory
    from games.tomodachi.tools.extract_shared_fonts import Slice, nca_romfs
    from games.tomodachi.tools.extract_romfs import index_romfs
    keys = load_keys(keys_path)
    folder = work / "control"
    folder.mkdir(exist_ok=True)
    with source.open("rb") as stream:
        _, entries = container_entries(stream, source.stat().st_size)
        tickets = common_tickets(stream, entries)
        controls = [r for r, _ in nca_inventory(stream, entries, keys)
                    if r["content_type"] == 2 and r["title_id"] == TITLE_ID]
    if len(controls) != 1:
        raise ValueError("Expected exactly one Tomodachi Control NCA")
    with tempfile.TemporaryDirectory(dir=folder) as temporary:
        image = Path(temporary) / "romfs.bin"
        nca_romfs(Slice(source, controls[0]["nca_offset"]), keys, image, tickets)
        rows = [r for r in index_romfs(image) if r["path"] == "control.nacp"]
        if len(rows) != 1 or rows[0]["size"] != 0x4000:
            raise ValueError("Invalid Control NACP")
        with image.open("rb") as stream:
            stream.seek(rows[0]["offset"])
            (folder / "control.nacp").write_bytes(stream.read(0x4000))
    verified_file(folder / "control.nacp", expected["control_nacp_sha256"])


def prepare(source, keys, work, *, romfs=False, resume=False):
    source, keys = source.expanduser().resolve(), keys.expanduser().resolve()
    work = outside_repository(work, ROOT)
    if work.exists() and not resume:
        raise ValueError("Use a new external work directory; existing results are never overwritten")
    identity = select_program(source, keys, TITLE_ID)
    if identity["package_kind"] != "base" or identity["cnmt_version_raw"] != 0:
        raise ValueError("Only Tomodachi base version 1.0.0 is currently accepted")
    stamp = (source.stat().st_size, source.stat().st_mtime_ns)
    work.mkdir(parents=True, exist_ok=resume)
    (work / "logs").mkdir(exist_ok=resume)
    report = {"identity": identity, "status": "preparing", "native_execution": "not_tested"}

    def step(name, script, *args):
        report.setdefault("tool_sha256", {})[script] = verified_file(HERE / "tools" / script)
        print(f"[{name}]", flush=True)
        with (work / "logs" / f"{name}.log").open("w") as log:
            result = subprocess.run([sys.executable, str(HERE / "tools" / script), *map(str, args)],
                                    stdout=log, stderr=subprocess.STDOUT,
                                    env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        if result.returncode:
            raise ValueError(f"{name} failed; see {work / 'logs' / (name + '.log')}")

    try:
        if resume and (work / "exefs").exists():
            manifest = json.loads((work / "extraction-manifest.json").read_text())
            if manifest["title_id"] != TITLE_ID or Path(manifest["source"]).resolve() != source or (manifest["source_size"], manifest["source_mtime_ns"]) != stamp:
                raise ValueError("Resume inputs differ from the original extraction")
            for row in manifest["files"]:
                verified_file(work / "exefs" / row["name"], row["sha256"])
        else:
            step("extract", "extract_exefs.py", source, "--keys", keys, "--title-id", TITLE_ID,
                 "--output", work / "exefs")
        expected = check_exefs(work)
        prepare_control(source, keys, work, expected)
        step("inventory", "prepare_modules.py", work / "exefs", "--output", work / "inventory")
        step("dependencies", "map_dependencies.py", work / "inventory", "--output", work / "dependencies.json")
        if not (resume and (work / "rewritten").exists()):
            step("rewrite", "rewrite_modules.py", "--exefs", work / "exefs", "--manifest",
                 work / "extraction-manifest.json", "--output", work / "rewritten")
        with tempfile.TemporaryDirectory(prefix="preflight-", dir=work) as temporary:
            output = Path(temporary) / "runtime"
            step("preflight", "runtime_preflight.py", "--exefs", work / "exefs", "--manifest",
                 work / "extraction-manifest.json", "--rewrites", work / "rewritten", "--output", output)
            (work / "runtime").mkdir(exist_ok=True)
            for result in output.iterdir():
                shutil.copy2(result, work / "runtime" / result.name)
        if romfs and resume and (work / "romfs/index.json").exists():
            index = json.loads((work / "romfs/index.json").read_text())
            verified_file(work / "romfs/romfs.bin", index["romfs_sha256"])
        elif romfs:
            step("romfs", "extract_romfs.py", "--keys", keys, "--manifest",
                 work / "extraction-manifest.json", "--output", work / "romfs")
        if romfs:
            index = json.loads((work / "romfs/index.json").read_text())
            if index["romfs_sha256"] != expected["romfs_sha256"] or index["romfs_size"] != expected["romfs_size"]:
                raise ValueError("RomFS differs from the pinned game build")
        if stamp != (source.stat().st_size, source.stat().st_mtime_ns):
            raise ValueError("Source changed during preparation")
        report["status"] = "prepared_for_native_host"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (work / "preparation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Prepared {work}. Native execution remains unverified.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--keys", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--romfs", action="store_true", help="also extract and verify the game data")
    parser.add_argument("--resume", action="store_true", help="revalidate and reuse a previous preparation")
    args = parser.parse_args()
    try:
        prepare(args.source, args.keys, args.work, romfs=args.romfs, resume=args.resume)
    except (ValueError, OSError, KeyError) as error:
        parser.exit(2, f"Tomodachi preparation failed: {error}\n")
