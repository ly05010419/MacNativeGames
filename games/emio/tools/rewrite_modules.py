#!/usr/bin/env python3
"""Probe generic NSO instruction rewriting from a verified external ExeFS."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
if __package__:
    from .prepare_modules import discover_nso_modules
    from .common import outside_repository, verified_file
else:
    from prepare_modules import discover_nso_modules
    from common import outside_repository, verified_file

REWRITER = HERE.parent / "third_party/d2r/patch_nso.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validated_inputs(exefs: Path, manifest: Path) -> tuple[str, list[str], dict]:
    verified_file(manifest)
    source = json.loads(manifest.read_text())
    require(isinstance(source, dict), "Invalid extraction manifest")
    title_id = source.get("title_id")
    require(isinstance(title_id, str) and re.fullmatch(r"[0-9a-fA-F]{16}", title_id),
            "Extraction manifest needs a 16-digit title ID")
    rows = source.get("files")
    require(isinstance(rows, list) and all(isinstance(row, dict) and
                                          isinstance(row.get("name"), str) for row in rows),
            "Invalid extraction manifest files")
    files = {row.get("name"): row for row in rows}
    require(len(files) == len(rows), "Duplicate extraction manifest filenames")
    modules = discover_nso_modules(exefs)
    for name in modules:
        row = files.get(name)
        require(isinstance(row, dict) and type(row.get("size")) is int and row["size"] > 0 and
                isinstance(row.get("sha256"), str) and
                re.fullmatch(r"[0-9a-f]{64}", row["sha256"]),
                f"Missing valid extraction hash for {name}")
        path = exefs / name
        require(path.stat().st_size == row["size"], f"Size mismatch: {name}")
        verified_file(path, row["sha256"])
    return title_id.upper(), modules, files


def count_rows(path: Path, build_id: str | None = None) -> int:
    lines = path.read_text().splitlines()
    if build_id is not None:
        require(bool(lines) and lines[0] == f"# BUILD_ID\t{build_id[:32]}",
                f"Rewrite build ID mismatch: {path.name}")
    return sum(bool(line.strip()) and not line.startswith("#") for line in lines)


def rewrite_exefs(exefs: Path, manifest: Path, output: Path) -> dict:
    require(not Path(output).expanduser().is_symlink(), "Output is a symlink")
    exefs = outside_repository(Path(exefs), ROOT)
    manifest = outside_repository(Path(manifest), ROOT)
    output = outside_repository(Path(output), ROOT)
    require(not output.exists() and not output.is_symlink(), "Output already exists")
    require(not output.is_relative_to(exefs), "Output cannot be inside the ExeFS input")
    title_id, modules, files = validated_inputs(exefs, manifest)
    rewriter_hash = verified_file(REWRITER)
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {"format": 1, "title_id": title_id, "rewriter_sha256": rewriter_hash,
              "modules": {}, "runtime": "not_assessed",
              "scope": "Instruction rewriting only; no loader, services, NVN or gameplay verification"}
    with tempfile.TemporaryDirectory(prefix="mng-rewrite-", dir=output.parent) as temp:
        stage = Path(temp)
        for name in modules:
            source = exefs / name
            destination = stage / name
            command = [sys.executable, str(REWRITER), str(source), str(destination), name]
            environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1",
                           "DM_RULESET": "native", "DM_PRIVATE_GATES": "0"}
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=environment, check=False)
            require(result.returncode == 0,
                    f"Rewrite failed for {name}: {(result.stderr or result.stdout)[-500:].strip()}")
            patched = destination / f"{name}.patched.bin"
            fixups = destination / f"{name}.fixups.tsv"
            require(fixups.is_file() and not fixups.is_symlink(),
                    f"Missing rewrite fixups: {name}")
            gates_path = destination / f"{name}.gates.tsv"
            fixups_hash = verified_file(fixups)
            gates_hash = verified_file(gates_path)
            with source.open("rb") as stream:
                original = stream.read(0x60)
            require(len(original) == 0x60, f"Truncated NSO header: {name}")
            build_id = original[0x40:0x60].hex()
            counts = {kind: count_rows(destination / f"{name}.{kind}.tsv",
                                       build_id if kind != "gates" else None)
                      for kind in ("gates", "cuts", "params", "obs")}
            require(not any(counts[kind] for kind in ("cuts", "params", "obs")),
                    f"Unexpected game-specific rewrite for {name}")
            report["modules"][name] = {
                "build_id": build_id, "nso_sha256": verified_file(source, files[name]["sha256"]),
                "patched_sha256": verified_file(patched),
                "fixups_sha256": fixups_hash, "gates_sha256": gates_hash, **counts,
            }
        (stage / "rewrite-summary.json").write_text(json.dumps(report, indent=2) + "\n")
        require(not output.exists() and not output.is_symlink(), "Output already exists")
        stage.rename(output)
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = rewrite_exefs(args.exefs, args.manifest, args.output)
    except (OSError, ValueError) as error:
        print(f"Rewrite probe failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
