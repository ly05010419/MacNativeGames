#!/usr/bin/env python3
"""Prepare Mario Kart 8 Deluxe's whole-game shader seed from the player's own, verified RomFS and game modules.

    .venv/bin/python scripts/prepare_mk8d_shader_seed.py --work WORK

The shared tool (third_party/totk-scripts/prepare_shader_seed.py, Odyssey's copy of games/totk's) does the work: one
translator service per core, the translation and inventory caches beside the seed, the seed manifest. This game differs in:
- Chain: the runtime's (third_party/d2r_gfx/gfx/nvn/dmsh_chain.h), not the tool's.
- Where shaders are: in every course's and object's archives (Yaz0 SARC, embedded BNSH), in the agl packages and in main,
  not in a few global archives. Every file is scanned, by one process per core: the shared Yaz0 decoder is pure Python,
  and the 11.5 GiB of archives take about 40 s that way instead of 6 minutes. Each shader found gets its default-state
  translation. 2026-10-28: 29,717 shaders, including all 6,779 the runtime translated in play.
- SARC members without a name (the UI layout archives): named by their index. The shared reader rejects such an archive
  whole, which hid 29 UI shaders, 14 of them used in play.
- The bind states this game's own list names (../assets/shader_list.tsv), when it has one.
- The .variants rows of vertex shaders record depth_minus_w: the game selects NVN's near-is-minus-w depth mode, and
  totk_rt_variant skips a vertex variant whose flag differs. shader-keys.json is left out: the importer accepts only the
  file names in totk_shader_seed.h and would reject the whole seed.

Output: <work>/build/shader-seed, which `make run` and the app (scripts/package_mk8d_app.py) import into their shader cache
on the next start. It holds data derived from your own game: never share or commit it.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys

GAME = Path(__file__).resolve().parents[1]
SHARED = GAME / "third_party/totk-scripts/prepare_shader_seed.py"
LIST = GAME / "assets/shader_list.tsv"
MIN_SHADERS = 25000   # 29,717 found in 4.0.1; far fewer means the scan went wrong
SEED_FILE = re.compile(r"^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$")


def runtime_chain():
    text = (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text()
    return re.search(r'#define DMSH_CHAIN "([^"]+)"', text).group(1)


def load_shared():
    sys.path.insert(0, str(SHARED.parent))
    spec = importlib.util.spec_from_file_location("shared_seed", SHARED)
    seed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seed)
    seed.CHAIN = runtime_chain()
    scope = seed.shader_key_scope   # its chain default was bound to the tool's chain when the module loaded
    seed.shader_key_scope = lambda keys, version, source, chain=None: scope(keys, version, source, chain=seed.CHAIN)
    seed.decoder = lambda romfs, read, game="totk": (lambda raw: seed.yaz0(raw) if raw[:4] == seed.YAZ0 else raw)
    seed.startup_package = lambda name, game="totk": True
    named = seed.sarc
    seed.sarc = lambda data: sarc_any(seed, named, data)
    seed.GAMES["totk"].update(folders=None, min_shaders=MIN_SHADERS, list=LIST, all_default=True)
    seed.inventory = inventory
    return seed


def sarc_any(seed, named, data):
    """The shared reader's members, or for an archive whose members carry no name (attribute high byte 0) #<index>."""
    try:
        return named(data)
    except ValueError:
        if data[:4] != b"SARC":
            raise
        endian = "<" if data[6:8] == b"\xff\xfe" else ">"
        header, size, start = struct.unpack_from(endian + "HxxII", data, 4)
        if size != len(data) or header < 20 or seed.checked(data, header, 4) != b"SFAT":
            raise
        count = struct.unpack_from(endian + "H", data, header + 6)[0]
        members = {}
        for i in range(count):
            _, attribute, lo, hi = struct.unpack_from(endian + "4I", seed.checked(data, header + 12 + i * 16, 16))
            if attribute >> 24 or hi < lo:   # a named member, or a bad range: the shared reader's error stands
                raise
            members[f"#{i}"] = seed.checked(data, start + lo, hi - lo)
        return members


SEED = None   # the shared module, loaded once per process (the scan's worker processes import this file again)


def shared():
    global SEED
    if SEED is None:
        SEED = load_shared()
    return SEED


def visit(name, data, found, depth=0):
    """The shared inventory's walk (prepare_shader_seed.py inventory.visit): agl packages, SARC members, embedded BNSH."""
    seed = shared()
    if data[:4] == b"BAHS":
        for key, stage, control, code in seed.sharcb_shaders(data):
            found.setdefault(key, (stage, control, code, name))
    if data[:4] == b"SARC":
        if depth > 4:
            return
        for member, raw in seed.sarc(data).items():
            visit(name + "/" + member, seed.yaz0(raw) if raw[:4] == seed.YAZ0 else raw, found, depth + 1)
        return
    pos = 0
    while (pos := data.find(b"BNSH\0\0\0\0", pos)) >= 0:
        size = struct.unpack_from("<I", seed.checked(data, pos + 28, 4))[0]
        for key, stage, control, code in seed.bnsh_shaders(seed.checked(data, pos, size)):
            found.setdefault(key, (stage, control, code, name))
        pos += max(size, 8)


def scan(job):
    """One worker's share of the RomFS: [(path, offset, size)] -> {path: (raw SHA-256, {key: shader})}."""
    romfs, rows = job
    seed, out = shared(), {}
    with (Path(romfs) / "romfs.bin").open("rb") as stream:
        for name, offset, size in rows:
            stream.seek(offset)
            raw = stream.read(size)
            if len(raw) != size:
                raise ValueError("short RomFS shader read")
            if raw[:4] not in (seed.ZSTD, seed.YAZ0, b"SARC", b"FSHA", b"BNSH", b"BAHS") and b"BNSH\0\0\0\0" not in raw:
                continue
            found = {}
            visit(name, seed.yaz0(raw) if raw[:4] == seed.YAZ0 else raw, found)
            if found:
                out[name] = (seed.digest(raw), found)
    return out


def inventory(romfs, modules=None, game="totk"):
    """The shared inventory(), over every file and by one process per core; the same result: (index, shaders, the keys
    translated for the default bind state, the archives). Shaders are taken in path order, as the shared one does."""
    index = json.loads((romfs / "index.json").read_text())
    if index.get("status") != "pass":
        raise ValueError("RomFS has not passed integrity verification")
    rows = sorted(((r["path"], r["offset"], r["size"]) for r in index["files"]), key=lambda r: -r[2])
    jobs = max(1, os.cpu_count() or 4)
    with ProcessPoolExecutor(jobs) as pool:   # the largest files first, dealt round-robin
        parts = list(pool.map(scan, [(str(romfs), rows[i::jobs * 4]) for i in range(jobs * 4)]))
    by_name = {name: result for part in parts for name, result in part.items()}
    shaders, archives = {}, []
    for name in sorted(by_name):
        sha, found = by_name[name]
        for key, shader in found.items():
            shaders.setdefault(key, shader)
        archives.append(dict(path=name, sha256=sha))
    for module in sorted(Path(modules).glob("*.patched.bin")) if modules else []:
        found = {}
        visit("module:" + module.name[:-len(".patched.bin")], module.read_bytes(), found)
        for key, shader in found.items():
            shaders.setdefault(key, shader)
    print(f"discovered {len(shaders)} unique shaders, {len(shaders)} of them translated for the default bind state", flush=True)
    if not shaders:
        raise ValueError("no shaders discovered")
    return index, shaders, set(shaders), archives


def finish(output: Path, chain: str, preparation: str):
    """Mark vertex variants with the depth mode, drop what the importer rejects, re-hash the manifest."""
    vertex = 0
    for index in sorted(output.glob("*.variants")):
        rows = json.loads(index.read_text())
        for row in rows:
            if json.loads((output / f"{row['variant']}_b.json").read_text()).get("stage") == "vs":
                row["depth_minus_w"] = True
                vertex += 1
        index.write_text(json.dumps(rows, separators=(",", ":")) + "\n")
    (output / "shader-keys.json").unlink(missing_ok=True)
    manifest = json.loads((output / "seed-manifest.json").read_text())
    if manifest.get("chain") != chain:
        raise SystemExit(f"seed chain {manifest.get('chain')} is not the runtime's {chain}")
    stray = sorted(p.name for p in output.iterdir() if p.name != "seed-manifest.json" and not SEED_FILE.match(p.name))
    if stray:
        raise SystemExit(f"unexpected files in the seed: {stray[:5]}")
    manifest["files"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(output.iterdir()) if SEED_FILE.match(p.name)}
    manifest["mk8d_preparation_sha256"] = preparation
    (output / "seed-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return vertex, len(manifest["files"])


def identity(seed, romfs: Path, translator: Path, spirv_cross: Path):
    """What a prepared seed must have been made from to be reused as it is."""
    return dict(chain=seed.CHAIN, source_romfs_sha256=json.loads((romfs / "index.json").read_text())["romfs_sha256"],
                translator_sha256=seed.digest(translator.read_bytes()), spirv_cross_sha256=seed.digest(spirv_cross.read_bytes()),
                preparation_sha256=seed.digest(SHARED.read_bytes()), mk8d_preparation_sha256=seed.digest(Path(__file__).read_bytes()),
                shader_list_sha256=seed.digest(LIST.read_bytes()) if LIST.is_file() else None)


def up_to_date(output: Path, want: dict) -> bool:
    """A finished seed made from the same inputs, every file it lists present (the importer checks their hashes)."""
    try:
        manifest = json.loads((output / "seed-manifest.json").read_text())
    except (OSError, ValueError):
        return False
    return all(manifest.get(k) == v for k, v in want.items()) and all((output / name).is_file() for name in manifest["files"])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", type=Path, required=True, help="the build folder (scripts/build_mk8d.py --work)")
    p.add_argument("--output", type=Path, help="the seed folder (default: <work>/build/shader-seed)")
    a = p.parse_args()
    work = a.work.expanduser().resolve()
    output = (a.output or work / "build/shader-seed").expanduser().resolve()
    romfs, modules = work / "extracted/local/romfs", work / "rewritten"
    translator, spirv_cross = work / "build/shader-service/totk_xlate", work / "build/spirv-cross/spirv-cross"
    for need in (romfs / "index.json", translator, spirv_cross):
        if not need.exists():
            raise SystemExit(f"missing {need}; run scripts/build_mk8d.py first")
    seed = shared()
    want = identity(seed, romfs, translator, spirv_cross)
    if up_to_date(output, want):
        print(f"shader seed up to date: {json.loads((output / 'seed-manifest.json').read_text())['shaders']} shaders")
        return
    report = seed.prepare(romfs, output, translator, spirv_cross, json.loads((GAME / "assets/expected-4.0.1.json").read_text())["version"],
                          modules=modules, game="totk")
    vertex, files = finish(output, seed.CHAIN, want["mk8d_preparation_sha256"])
    print(f"shader seed: {report['shaders']} variants of {report['discovered']} shaders, {len(report['skipped'])} not translated; "
          f"{vertex} vertex variants; {files} files in the manifest")


if __name__ == "__main__":
    main()
