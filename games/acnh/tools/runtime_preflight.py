#!/usr/bin/env python3
"""Decode verified rewritten NSO tables without claiming a runnable native port."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import struct
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "third_party/d2r"))

if __package__:
    from .rewrite_modules import REWRITER, require, validated_inputs
    from .common import outside_repository, verified_file
else:
    from rewrite_modules import REWRITER, require, validated_inputs
    from common import outside_repository, verified_file
from relr import decode_relr


SUPPORTED_RELOCATIONS = {257, 1025, 1026, 1027}


def checked_unpack(fmt: str, image: bytes, offset: int, limit: int):
    size = struct.calcsize(fmt)
    require(0 <= offset <= limit - size and offset + size <= len(image),
            f"Table offset outside image: {offset:#x}")
    return struct.unpack_from(fmt, image, offset)


def parse_fixups(path: Path, image_size: int) -> dict[str, int]:
    verified_file(path)
    layout = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        require(len(parts) == 3 and parts[0] == "#" and parts[1] not in layout,
                f"Invalid rewrite fixups: {path.name}")
        layout[parts[1]] = int(parts[2], 0)
    keys = ("TEXT_END", "RO_OFF", "DATA_OFF", "KEY_PAGE_OFF", "KEY_PAGE_SIZE", "PATCH_OFF")
    require(all(key in layout for key in keys), f"Incomplete rewrite fixups: {path.name}")
    require(0 < layout["TEXT_END"] <= layout["RO_OFF"] <= layout["DATA_OFF"] <
            layout["KEY_PAGE_OFF"] and 0 < layout["KEY_PAGE_SIZE"] and
            layout["KEY_PAGE_OFF"] + layout["KEY_PAGE_SIZE"] <= layout["PATCH_OFF"] <= image_size,
            f"Invalid rewrite layout: {path.name}")
    return layout


def parse_dynamic(image: bytes, layout: dict) -> dict[int, int]:
    limit = layout["KEY_PAGE_OFF"]
    mod0, = checked_unpack("<I", image, 4, limit)
    require(image[mod0:mod0 + 4] == b"MOD0", "Missing MOD0 header")
    relative, = checked_unpack("<i", image, mod0 + 4, limit)
    dynamic = mod0 + relative
    require(layout["DATA_OFF"] <= dynamic < limit, "Dynamic table outside writable image")
    tags = {}
    for index in range(4096):
        tag, value = checked_unpack("<qQ", image, dynamic + index * 16, limit)
        if tag == 0:
            return tags
        # DT_NEEDED is an ordered, repeatable dependency entry, not a singleton
        # loader-address tag. prepare_modules preserves its names in `needed`.
        # ACNH main depends on both nnSdk and Multimedia; neither is a duplicate
        # definition of a symbol/relocation table.
        if tag == 1:
            continue
        require(tag not in tags, f"Duplicate dynamic tag: {tag}")
        tags[tag] = value
    raise ValueError("Unterminated dynamic table")


def parse_symbols(image: bytes, raw_nso: bytes, layout: dict, tags: dict) -> list[dict]:
    limit = layout["KEY_PAGE_OFF"]
    require(tags.get(11) == 24 and all(tag in tags for tag in (5, 6, 10)),
            "Unsupported dynamic symbol table")
    string_address, string_size = tags[5], tags[10]
    require(0 < string_size <= 16 * 1024 * 1024 and string_address + string_size <= limit,
            "Dynamic string table outside image")
    strings = image[string_address:string_address + string_size]
    require(len(raw_nso) >= 0xA0, "Truncated NSO symbol header")
    _, _, symbol_offset, symbol_size = struct.unpack_from("<IIII", raw_nso, 0x90)
    require(layout["RO_OFF"] + symbol_offset == tags[6] and symbol_size % 24 == 0 and
            symbol_size > 0, "Cannot establish dynamic symbol count")
    nso_count = symbol_size // 24
    if 4 in tags:
        _, count = checked_unpack("<II", image, tags[4], limit)
        require(count == nso_count, "Symbol count differs from NSO extent")
    else:
        count = nso_count
    require(0 < count <= 1_000_000 and tags[6] + count * 24 <= limit,
            "Dynamic symbol table outside image")
    symbols = []
    for index in range(count):
        offset, info, _, section, value, size = checked_unpack("<IBBHQQ", image, tags[6] + index * 24, limit)
        require(offset < len(strings), "Invalid dynamic symbol name offset")
        end = strings.find(b"\0", offset)
        require(end >= 0, "Unterminated dynamic symbol name")
        try:
            name = strings[offset:end].decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError("Invalid UTF-8 dynamic symbol name") from error
        symbols.append({"name": name,
                        "value": value, "size": size, "defined": bool(section),
                        "binding": info >> 4, "type": info & 15})
    return symbols


def parse_relocations(image: bytes, layout: dict, tags: dict, symbol_count: int) -> list[dict]:
    limit = layout["KEY_PAGE_OFF"]
    require(not any(tag in tags for tag in (17, 18, 19, 0x6000000F, 0x60000010,
                                            0x60000011, 0x60000012, 0x6FFFE000)),
            "Unsupported relocation table")
    require((not tags.get(8, 0) or tags.get(9) == 24) and
            (not tags.get(2, 0) or tags.get(20) == 7),
            "Unsupported RELA entry or PLT format")
    if any(tag in tags for tag in (35, 36, 37)):
        require(all(tag in tags for tag in (35, 36, 37)) and
                tags[36] + tags[35] <= limit,
                "RELR table outside original image")
    result = [{"where": where, "kind": 1027, "symbol": 0, "addend": addend}
              for where, addend in decode_relr(image, tags, layout["DATA_OFF"], limit)]
    seen = {row["where"] for row in result}
    for address_tag, size_tag in ((7, 8), (23, 2)):
        address, size = tags.get(address_tag), tags.get(size_tag, 0)
        require(not size or address is not None, "Missing RELA table address")
        require(size % 24 == 0 and size <= 24_000_000, "Invalid RELA table size")
        if not size:
            continue
        require(address + size <= limit, "RELA table outside image")
        for offset in range(address, address + size, 24):
            where, info, addend = checked_unpack("<QQq", image, offset, limit)
            kind, symbol = info & 0xFFFFFFFF, info >> 32
            require(kind in SUPPORTED_RELOCATIONS, f"Unsupported relocation kind: {kind}")
            require(symbol < symbol_count, "Relocation symbol index outside table")
            require(where % 8 == 0 and layout["DATA_OFF"] <= where <= limit - 8,
                    "Relocation target outside writable image")
            require(where not in seen, "Duplicate relocation target")
            require(kind != 1027 or 0 <= addend < limit, "Relative addend outside module")
            seen.add(where)
            result.append({"where": where, "kind": kind, "symbol": symbol, "addend": addend})
    return result


def parse_gates(path: Path, image_size: int, layout: dict) -> list[dict]:
    verified_file(path)
    gates = []
    returns = set()
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        parts = line.split()
        require(len(parts) == 4 and parts[1] in ("cntpct", "cntvct", "svc"),
                f"Invalid gate row: {path.name}")
        ret, arg, original = int(parts[0], 0), int(parts[2], 0), int(parts[3], 0)
        require((0 <= arg <= 0xFFFF if parts[1] == "svc" else 0 <= arg <= 31),
                f"Invalid gate argument: {path.name}")
        require(ret % 4 == 0 and layout["PATCH_OFF"] <= ret <= image_size - 4 and
                original % 4 == 0 and 0 <= original < layout["TEXT_END"],
                f"Gate outside patch area or original text: {path.name}")
        require(ret not in returns, f"Duplicate gate return: {path.name}")
        returns.add(ret)
        gates.append({"return": ret, "kind": parts[1], "arg": arg, "original": original})
    return gates


def module_data(name: str, exefs: Path, rewrites: Path, source_row: dict) -> dict:
    source = exefs / name
    folder = rewrites / name
    patched = folder / f"{name}.patched.bin"
    require(verified_file(source, source_row["nso_sha256"]) == source_row["nso_sha256"],
            f"NSO hash mismatch: {name}")
    verified_file(patched, source_row["patched_sha256"])
    image = patched.read_bytes()
    raw_nso = source.read_bytes()[:0xA0]
    require(raw_nso[0x40:0x60].hex() == source_row["build_id"],
            f"NSO build ID mismatch: {name}")
    layout = parse_fixups(folder / f"{name}.fixups.tsv", len(image))
    tags = parse_dynamic(image, layout)
    symbols = parse_symbols(image, raw_nso, layout, tags)
    relocations = parse_relocations(image, layout, tags, len(symbols))
    gates = parse_gates(folder / f"{name}.gates.tsv", len(image), layout)
    require(len(gates) == source_row["gates"], f"Gate count mismatch: {name}")
    return {"nso_sha256": source_row["nso_sha256"],
            "patched_sha256": source_row["patched_sha256"], "build_id": source_row["build_id"],
            "file_size": len(image), "layout": layout, "dynamic_tags": tags,
            "symbols": symbols, "relocations": relocations, "gates": gates}


def summarize_linkage(modules: dict, module_order: list[str]) -> dict:
    # The loader's per-module hash keeps the first defined symbol for a name.
    # Its global search then checks the modules in runtime load order.
    first_definitions = {}
    for name in module_order:
        definitions = {}
        for index, symbol in enumerate(modules[name]["symbols"]):
            if index and symbol["defined"]:
                definitions.setdefault(symbol["name"], symbol["value"])
        first_definitions[name] = definitions
    global_candidates = set()
    for name in module_order:
        limit = modules[name]["layout"]["KEY_PAGE_OFF"]
        global_candidates.update(symbol for symbol, value in first_definitions[name].items()
                                 if value < limit)

    result = {}
    for name in module_order:
        module = modules[name]
        limit = module["layout"]["KEY_PAGE_OFF"]
        counts = {"symbolic_relocations": 0, "local_candidate": 0,
                  "module_candidate": 0, "weak_zero_candidate": 0,
                  "host_required_candidate": 0}
        weak_names, required_names = set(), set()
        for relocation in module["relocations"]:
            if relocation["kind"] == 1027:
                continue
            counts["symbolic_relocations"] += 1
            symbol = module["symbols"][relocation["symbol"]]
            if symbol["defined"] and symbol["binding"] != 2 and symbol["value"] < limit:
                counts["local_candidate"] += 1
            elif not (symbol["defined"] and symbol["binding"] != 2) and \
                    symbol["name"] in global_candidates:
                counts["module_candidate"] += 1
            elif symbol["binding"] == 2:
                counts["weak_zero_candidate"] += 1
                weak_names.add(symbol["name"])
            else:
                counts["host_required_candidate"] += 1
                required_names.add(symbol["name"])
        result[name] = {**counts, "weak_zero_names": sorted(weak_names),
                        "host_required_names": sorted(required_names)}
    return result


def prepare(exefs: Path, manifest: Path, rewrites: Path, output: Path) -> dict:
    require(not Path(output).expanduser().is_symlink(), "Output is a symlink")
    exefs = outside_repository(Path(exefs), ROOT)
    manifest = outside_repository(Path(manifest), ROOT)
    rewrites = outside_repository(Path(rewrites), ROOT)
    output = outside_repository(Path(output), ROOT)
    require(not output.exists() and not output.is_symlink(), "Output already exists")
    require(not output.is_relative_to(exefs) and not output.is_relative_to(rewrites),
            "Output cannot be inside an input directory")
    title_id, discovered, files = validated_inputs(exefs, manifest)
    require(all(re.fullmatch(r"(?:rtld|main|sdk|subsdk[0-9]+)", name) for name in discovered),
            "Unsupported NSO module role")
    summary_path = rewrites / "rewrite-summary.json"
    verified_file(summary_path)
    summary = json.loads(summary_path.read_text())
    require(isinstance(summary, dict) and summary.get("title_id") == title_id and
            isinstance(summary.get("modules"), dict) and
            set(summary["modules"]) == set(discovered), "Rewrite module set or title mismatch")
    require(summary.get("rewriter_sha256") == verified_file(REWRITER),
            "Rewriter changed since the rewrite probe")
    for name in discovered:
        row = summary["modules"][name]
        require(isinstance(row, dict) and row.get("nso_sha256") == files[name]["sha256"] and
                all(row.get(key) == 0 for key in ("cuts", "params", "obs")),
                f"Rewrite source or generic scope mismatch: {name}")
        require(isinstance(row.get("patched_sha256"), str) and
                re.fullmatch(r"[0-9a-f]{64}", row["patched_sha256"]),
                f"Invalid rewrite digest: {name}")
        require(isinstance(row.get("build_id"), str) and
                re.fullmatch(r"[0-9a-f]{64}", row["build_id"]) and
                type(row.get("gates")) is int and row["gates"] >= 0,
                f"Invalid rewrite metadata: {name}")
        for kind in ("fixups", "gates"):
            digest = row.get(f"{kind}_sha256")
            require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest),
                    f"Invalid rewrite {kind} digest: {name}")
            verified_file(rewrites / name / f"{name}.{kind}.tsv", digest)
    loadable = ["main"] + sorted((name for name in discovered if name.startswith("subsdk")),
                                 key=lambda name: int(name[6:]))
    if "sdk" in discovered:
        loadable.append("sdk")
    for name in discovered:
        if name in loadable:
            continue
        row = summary["modules"][name]
        with (exefs / name).open("rb") as source:
            header = source.read(0x60)
        require(header[0x40:0x60].hex() == row.get("build_id"),
                f"NSO build ID mismatch: {name}")
        patched = rewrites / name / f"{name}.patched.bin"
        verified_file(patched, row.get("patched_sha256"))
        layout = parse_fixups(rewrites / name / f"{name}.fixups.tsv", patched.stat().st_size)
        gates = parse_gates(rewrites / name / f"{name}.gates.tsv", patched.stat().st_size, layout)
        require(len(gates) == row.get("gates"), f"Gate count mismatch: {name}")
    modules = {name: module_data(name, exefs, rewrites, summary["modules"][name])
               for name in loadable}
    report = {"format": 1, "title_id": title_id, "module_order": loadable,
              "excluded_modules": [name for name in discovered if name not in loadable],
              "linkage": summarize_linkage(modules, loadable),
              "linkage_scope": "Guest address candidates for referenced relocations only; "
                               "host overrides, ABI and runtime use not assessed",
              "modules": {name: {"symbols": len(data["symbols"]),
                                 "relocations": len(data["relocations"]),
                                 "relocation_kinds": dict(Counter(str(row["kind"])
                                                                  for row in data["relocations"])),
                                 "encoded_relocation_bytes": sum(data["dynamic_tags"].get(tag, 0)
                                                                 for tag in (35, 8, 2)),
                                 "flat_c_table_estimate_bytes": 24 * len(data["relocations"]),
                                 "gates": len(data["gates"])} for name, data in modules.items()},
              "runtime": "not_assessed",
              "scope": "Static loader table data only; constructors, host bindings, NVN and gameplay unverified"}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mng-runtime-", dir=output.parent) as temporary:
        stage = Path(temporary)
        (stage / "runtime-modules.json").write_text(json.dumps({"summary": report, "modules": modules}) + "\n")
        (stage / "runtime-summary.json").write_text(json.dumps(report, indent=2) + "\n")
        require(not output.exists() and not output.is_symlink(), "Output already exists")
        stage.rename(output)
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("exefs", "manifest", "rewrites", "output"):
        parser.add_argument(f"--{flag}", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = prepare(args.exefs, args.manifest, args.rewrites, args.output)
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"Runtime preflight failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
