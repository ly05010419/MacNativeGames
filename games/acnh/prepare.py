#!/usr/bin/env python3
"""Prepare pinned ACNH inputs for native-host bring-up; does not launch the game."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLS = HERE / "tools"
sys.path.insert(0, str(ROOT))

from games.acnh.tools.common import outside_repository, sha256_file, verified_file
from games.acnh.tools.identity import select_program

TITLE_ID = "01006F8002326000"
SERVICE_GROUPS = ("nn::time", "nn::bcat", "nn::fs", "nn::account", "nn::nifm",
                  "nn::ldn", "nn::socket", "nn::friends", "nn::nfp", "nn::prepo")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text())


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def profile(version):
    return read_json(HERE / "assets" / f"expected-v{version}.json")


def check_identity(identity, expected):
    for key in ("title_id", "package_kind", "cnmt_version_raw"):
        require(identity.get(key) == expected[key], f"Unsupported {key}: {identity.get(key)}")


def check_exefs(manifest, expected):
    require(manifest.get("title_id") == TITLE_ID, "Extracted title ID mismatch")
    rows = manifest["files"]
    hashes = {row["name"]: row["sha256"] for row in rows}
    require(len(hashes) == len(rows), "Duplicate ExeFS manifest rows")
    require(hashes == expected["exefs_sha256"], "Unknown ExeFS build; exact pinned hashes required")


def check_modules(modules, expected):
    actual = {row["module"]: row["build_id"] for row in modules}
    want = {row["name"]: row["build_id"] for row in expected["modules"]}
    require(len(actual) == len(modules) and actual == want, "NSO module set/build IDs differ")
    require(all(row["segment_hashes_verified"] for row in modules), "NSO segment hashes missing")


def service_audit(dependencies):
    imports = dependencies["modules"]["main"]["imports"]
    return {
        "scope": "Static imports only; runtime use and host coverage are unverified",
        "groups": {group: [row["demangled"] for row in imports if row["group"] == group]
                   for group in SERVICE_GROUPS},
    }


def control_identity(source, keys_path, folder, expected):
    # Reuse the bounded Control RomFS reader and IVFC verifier.
    from games.acnh.tools.extract_exefs import load_keys, container_entries, common_tickets, nca_inventory
    from games.acnh.tools.extract_romfs import index_romfs
    from games.acnh.tools.extract_shared_fonts import Slice, nca_romfs
    keys = load_keys(keys_path)
    with source.open("rb") as stream:
        _, entries = container_entries(stream, source.stat().st_size)
        tickets = common_tickets(stream, entries)
        controls = [record for record, _ in nca_inventory(stream, entries, keys)
                    if record["content_type"] == 2 and record["title_id"] == TITLE_ID]
    require(len(controls) == 1, "Expected exactly one Control NCA for ACNH")
    with tempfile.TemporaryDirectory(prefix="control-", dir=folder) as temporary:
        image = Path(temporary) / "romfs.bin"
        nca_romfs(Slice(source, controls[0]["nca_offset"]), keys, image, tickets)
        rows = [row for row in index_romfs(image) if row["path"] == "control.nacp"]
        require(len(rows) == 1 and rows[0]["size"] == 0x4000, "Invalid Control NACP")
        with image.open("rb") as stream:
            stream.seek(rows[0]["offset"])
            nacp = stream.read(rows[0]["size"])
    target = folder / "control.nacp"
    target.write_bytes(nacp)
    digest = verified_file(target, expected["control_nacp_sha256"])
    display = nacp[0x3060:0x3070].split(b"\0", 1)[0].decode("utf-8")
    require(display == expected["display_version"], "Control display version mismatch")
    return {"display_version": display, "control_nacp_sha256": digest,
            "integrity": "Control FS header and all six IVFC levels verified"}


def run_step(work, name, script, *args):
    print(f"[{name}]", flush=True)
    with (work / "logs" / f"{name}.log").open("w") as log:
        result = subprocess.run([sys.executable, str(script), *map(str, args)],
                                stdout=log, stderr=subprocess.STDOUT,
                                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    require(result.returncode == 0, f"{name} failed; see {work / 'logs' / (name + '.log')}")


def prepare(base, update, keys, work, hactool=None):
    # Reject every checkout, including a path that reaches one through a symlink.
    work = outside_repository(work, ROOT)
    require(not work.exists(), "Work directory already exists; use a new external directory")
    for source in (base, update, keys):
        require(source.is_file() and not source.is_symlink(), f"Missing regular input: {source}")
    base, update, keys = (path.expanduser().resolve() for path in (base, update, keys))
    if hactool:
        verified_file(hactool)
        hactool = hactool.resolve()
    sources = {"base": base, "update": update}
    stamps = {role: (path.stat().st_size, path.stat().st_mtime_ns) for role, path in sources.items()}
    expected = {"base": profile(0), "update": profile(458752)}
    identities = {role: select_program(path, keys, TITLE_ID) for role, path in sources.items()}
    for role in sources:
        check_identity(identities[role], expected[role])

    work.mkdir(parents=True)
    (work / "logs").mkdir()
    summary = {"title_id": TITLE_ID, "status": "preparing", "inputs": identities, "control": {},
               "runtime": "not_assessed", "romfs": "not_assessed",
               "scope": "Input integrity, instruction rewriting and static loader preflight only"}
    write_json(work / "preparation.json", summary)
    try:
        for role, source in sources.items():
            folder = work / role
            run_step(work, f"extract-{role}", TOOLS / "extract_exefs.py", source,
                     "--keys", keys, "--title-id", TITLE_ID, "--output", folder / "exefs",
                     *(["--update"] if role == "update" else []))
            check_exefs(read_json(folder / "extraction-manifest.json"), expected[role])
            summary["control"][role] = control_identity(source, keys, folder, expected[role])
            run_step(work, f"inventory-{role}", TOOLS / "prepare_modules.py",
                     folder / "exefs", "--output", folder / "inventory")
            check_modules(read_json(folder / "inventory/modules.json"), expected[role])

        target = work / "update"
        run_step(work, "dependencies", TOOLS / "map_dependencies.py", target / "inventory",
                 "--output", target / "dependencies.json")
        write_json(work / "service-audit.json", service_audit(read_json(target / "dependencies.json")))
        run_step(work, "rewrite", TOOLS / "rewrite_modules.py", "--exefs", target / "exefs",
                 "--manifest", target / "extraction-manifest.json", "--output", target / "rewritten")
        run_step(work, "preflight", TOOLS / "runtime_preflight.py", "--exefs", target / "exefs",
                 "--manifest", target / "extraction-manifest.json", "--rewrites", target / "rewritten",
                 "--output", target / "runtime")
        summary["rewrite"] = read_json(target / "rewritten/rewrite-summary.json")
        summary["preflight"] = read_json(target / "runtime/runtime-summary.json")
        if hactool:
            summary["hactool_sha256"] = sha256_file(hactool)
            run_step(work, "romfs-merge", TOOLS / "extract_update_romfs.py", base, update,
                     "--keys", keys, "--title-id", TITLE_ID, "--hactool", hactool,
                     "--output", target / "romfs")
            index = read_json(target / "romfs/index.json")
            summary["romfs"] = {key: index[key] for key in ("status", "romfs_size", "romfs_sha256", "integrity")}
            summary["romfs"]["file_count"] = len(index["files"])
            if "romfs_sha256" in expected["update"]:
                require(index["romfs_sha256"] == expected["update"]["romfs_sha256"], "Merged RomFS hash differs")
        for role, path in sources.items():
            require((path.stat().st_size, path.stat().st_mtime_ns) == stamps[role], "Input changed during preparation")
        summary["status"] = "prepared_for_host_bringup"
    except Exception as error:
        summary.update(status="failed", error=str(error))
        write_json(work / "preparation.json", summary)
        raise
    write_json(work / "preparation.json", summary)
    print(f"Prepared: {work}\nNative execution, graphics and gameplay remain unverified.", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("--update", required=True, type=Path)
    parser.add_argument("--keys", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path, help="new directory outside all source checkouts")
    parser.add_argument("--hactool", type=Path, help="also merge/verify RomFS using the project's patched hactool")
    args = parser.parse_args()
    try:
        prepare(args.base.expanduser(), args.update.expanduser(), args.keys.expanduser(),
                args.work.expanduser(), args.hactool.expanduser() if args.hactool else None)
    except (OSError, ValueError, KeyError) as error:
        print(f"ACNH preparation failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
