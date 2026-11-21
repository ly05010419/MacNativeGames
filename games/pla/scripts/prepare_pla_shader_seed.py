#!/usr/bin/env python3
"""Prepare Pokemon Legends: Arceus's whole-game shader seed from the local, verified RomFS and game modules.

iOS cannot translate shaders while playing, so the iPhone / iPad build carries this seed (build/shader-seed, imported
by totk_shader_seed_ports.h totk_seed_import_into). The work is done by scripts/prepare_shader_seed.py (a copy of
Tears of the Kingdom's tool: inventory, one translator service per core, seed manifest), adapted here without changing
it, as games/odyssey/scripts/prepare_odyssey_shader_seed.py does for Odyssey (21 November 2026):
- Chain: this runtime's (third_party/d2r_gfx/gfx/nvn/dmsh_chain.h, v4), not the copy's v8.
- Keys: Tears of the Kingdom's rule (nvn_mtl.m prog_get: SHA-256 of control[:ctl_key_len] + code[:control+0x704]).
  Arceus binds its 0x880-byte control sections unmodified; Breath of the Wild's bind-time rewrite does not apply.
- Containers: no Zstandard dictionaries (Arceus has no Pack/ZsDic.pack.zs): raw data passes unchanged, a Zstandard
  frame without a dictionary id is decompressed, one that needs a dictionary is left as it is.
- Coverage: every shader found anywhere in the RomFS or the game modules gets its default-bind-state translation (the
  copy translates only Tears of the Kingdom's startup packages), plus the bind states assets/shader_list.tsv names.
- Requests: as this runtime's totk_shader_rt.m totk_rt_variant builds them. depth_minus_w is False (Arceus never selects
  NVN's near-is-minus-W depth mode: every vertex shader of the Mac app's cache translates identically only with False)
  and early_fragment_tests is never sent (this runtime does not send it). The *.variants rows carry no depth_minus_w,
  as the runtime writes them.
- Files: shader-keys.json is left out (the importer rejects any name outside its pattern, and with it the whole seed).
Output: an importable seed directory (default <work>/build/shader-seed). It contains game-derived data and stays in the
work folder, never in the repo.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

GAME = Path(__file__).resolve().parents[1]
WORK = Path(os.environ.get("PLA_WORK", "/Volumes/mac-win/totk-app/build/pla")).resolve()   # PLA_WORK: another work folder
VERSION = os.environ.get("PLA_VERSION", "v1101")   # the extracted/ and rewritten/ folder of the game version
UPSTREAM = GAME / "scripts/prepare_shader_seed.py"
SEED_NAME = re.compile(r"^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$")
DEPTH_MINUS_W = False   # hle_nvn.c: 0 unless the game calls nvnDeviceSetDepthMode(NEAR_IS_MINUS_W); Arceus does not
MIN_SHADERS = 1000      # a scan that finds fewer has missed containers (21 November 2026: see the report of a run)


def runtime_chain():
    text = (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text()
    found = re.search(r'#define DMSH_CHAIN "([^"]+)"', text)
    if not found:
        raise SystemExit("no DMSH_CHAIN in dmsh_chain.h")
    return found.group(1)


def pla_decoder(romfs, read, game="totk"):
    """Raw data unchanged; a Zstandard frame without a dictionary decompressed (none of Arceus's needs one)."""
    import compression.zstd as zstd

    def decode(raw):
        if raw[:4] != b"\x28\xb5\x2f\xfd":
            return raw
        info = zstd.get_frame_info(raw)
        if info.dictionary_id or info.decompressed_size is None or info.decompressed_size > 512 << 20:
            return raw
        return zstd.decompress(raw)
    return decode


def pla_translation_request(variant, stage, control, code, answers, code_path):
    """totk_shader_rt.m totk_rt_variant: variant, stage, code (graphics code without its 0x30-byte header), c1, the
    answers, local (compute: nvn_mtl.m rt_local, control+0x748) and depth_minus_w (False for Arceus). No
    early_fragment_tests: this runtime never sends it."""
    c1, length = struct.unpack_from("<II", control, 0x700)
    code_path.write_bytes(code[0 if stage == "cs" else 0x30:length])
    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(), **answers)
    if stage == "cs":
        request["local"] = list(struct.unpack_from("<III", control, 0x748))
    request["depth_minus_w"] = DEPTH_MINUS_W
    return request


def pla_early_fragment_tests(stage, control):
    """Never asked for: this runtime's requests carry no early_fragment_tests."""
    return False


def load_upstream():
    sys.path.insert(0, str(UPSTREAM.parent))
    spec = importlib.util.spec_from_file_location("upstream_seed", UPSTREAM)
    seed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seed)
    seed.CHAIN = runtime_chain()
    scope = seed.shader_key_scope   # its chain default was bound to the copy's chain when the module loaded
    seed.shader_key_scope = lambda keys, version, source, chain=None: scope(keys, version, source, chain=seed.CHAIN)
    seed.decoder = pla_decoder
    seed.startup_package = lambda name, game="totk": True   # every shader gets its default-state translation
    seed.translation_request = pla_translation_request     # both name the translation cache's identity (request_sha256)
    seed.early_fragment_tests = pla_early_fragment_tests
    seed.GAMES["totk"].update(folders=None, min_shaders=MIN_SHADERS, list=GAME / "assets/shader_list.tsv", all_default=True)
    return seed


def finish(seed, output: Path):
    """Drop the files the importer rejects, re-hash the manifest, and refresh the verified-digest record."""
    (output / "shader-keys.json").unlink(missing_ok=True)
    manifest = json.loads((output / "seed-manifest.json").read_text())
    if manifest.get("chain") != seed.CHAIN:
        raise SystemExit(f"seed chain {manifest.get('chain')} is not the runtime's {seed.CHAIN}")
    stray = [p.name for p in output.iterdir() if p.name != "seed-manifest.json" and not SEED_NAME.match(p.name)]
    if stray:
        raise SystemExit(f"unexpected files in the seed: {stray[:5]}")
    manifest["files"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(output.iterdir()) if SEED_NAME.match(p.name)}
    (output / "seed-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    seed.record_verified(output, manifest["files"])
    print(f"{len(manifest['files'])} files in the manifest", flush=True)


def check_importable(output: Path, chain: str):
    """totk_seed_import_into's rules, and every file's SHA-256. Returns the manifest."""
    manifest = json.loads((output / "seed-manifest.json").read_text())
    problems = []
    if not isinstance(manifest, dict) or manifest.get("chain") != chain or manifest.get("schema") != 1:
        problems.append("identity (chain / schema)")
    shaders, files = manifest.get("shaders"), manifest.get("files")
    if not isinstance(shaders, int) or isinstance(shaders, bool) or shaders <= 0:
        problems.append("shaders")
    if not isinstance(files, dict) or not files:
        problems.append("files")
    else:
        bad = [n for n in files if not SEED_NAME.match(n)]
        wrong = [n for n, sha in files.items() if not (output / n).is_file()
                 or hashlib.sha256((output / n).read_bytes()).hexdigest() != sha]
        unlisted = [p.name for p in output.iterdir() if p.name != "seed-manifest.json" and p.name not in files]
        problems += [f"{what}: {names[:3]}" for what, names in
                     (("unsafe names", bad), ("hash mismatch", wrong), ("files outside the manifest", unlisted)) if names]
    if problems:
        raise SystemExit("seed not importable: " + "; ".join(problems))
    return manifest


def compare_with_cache(output: Path, cache: Path):
    """The Mac app's translation cache against the seed: keys present, variants serving the same deps, and whether the
    serving variant's Metal is the cached one (variant names inside the source replaced)."""
    keys = matched = served = same = 0
    missing, differ = [], []
    for index in sorted(cache.glob("*.variants")):
        key = index.name[:-len(".variants")]
        keys += 1
        seeded = output / index.name
        if not seeded.is_file():
            missing.append(key); continue
        matched += 1
        rows = json.loads(seeded.read_text())
        for row in json.loads(index.read_text()):
            twin = next((r for r in rows if r["deps"] == row["deps"]), None)
            if twin is None:
                differ.append((row["variant"], "no seed variant with these deps")); continue
            served += 1
            want = (cache / f"{row['variant']}_b.metal").read_text()
            got = (output / f"{twin['variant']}_b.metal").read_text().replace(twin["variant"], row["variant"])
            if got == want:
                same += 1
            else:
                differ.append((row["variant"], f"Metal differs from seed {twin['variant']}"))
    print(f"Mac cache keys in the seed: {matched}/{keys}; cached variants with a same-deps seed variant: {served}; "
          f"identical Metal: {same}", flush=True)
    for key in missing:
        print(f"  key not in the seed: {key}")
    for variant, why in differ:
        print(f"  {variant}: {why}")
    return dict(keys=keys, matched=matched, served=served, same=same, missing=missing, differ=differ)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--output", type=Path, default=WORK / "build/shader-seed")
    p.add_argument("--finish-only", action="store_true", help="only re-run the fix-ups and checks on an existing seed")
    p.add_argument("--compare", type=Path, help="a translation cache (rt-<chain>) to check the seed against")
    a = p.parse_args()
    seed = load_upstream()
    started = time.monotonic()
    if not a.finish_only:
        seed.prepare(WORK / f"extracted/{VERSION}/romfs", a.output.resolve(), WORK / "build/shader-service/totk_xlate",
                     WORK / "build/spirv-cross/spirv-cross", VERSION, modules=WORK / f"rewritten/{VERSION}", game="totk")
    finish(seed, a.output)
    manifest = check_importable(a.output, seed.CHAIN)
    variants = sum(1 for _ in a.output.glob("*_b.metal"))
    size = sum(p.stat().st_size for p in a.output.iterdir())
    print(f"importable seed {a.output}: {manifest['shaders']} variants of {manifest.get('discovered')} discovered shaders "
          f"({sum(1 for _ in a.output.glob('*.variants'))} with an index, {variants} Metal files), "
          f"{len(manifest.get('skipped', []))} skipped, {len(manifest['files'])} files, {size / 1e6:.1f} MB; "
          f"{time.monotonic() - started:.0f} s", flush=True)
    if a.compare:
        compare_with_cache(a.output, a.compare)


if __name__ == "__main__":
    main()
