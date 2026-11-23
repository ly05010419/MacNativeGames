#!/usr/bin/env python3
"""Prepare Odyssey's whole-game shader seed from the local, verified RomFS and game modules.

Upstream's tool (third_party/totk-scripts/prepare_shader_seed.py, a copy of games/totk's) does the work: inventory, one translator service per core,
seed manifest. Odyssey needs four differences, applied here without changing the shared tool (22 October 2026):
- Keys: TotK's rule. Odyssey binds its 0x848-byte control sections unmodified, so BotW's bind-time rewrite must not
  be applied (with it, 182 of 844 runtime shaders matched; without it, 828; SHADER_STUTTER.md).
- Containers: Yaz0 (.szs), as for Breath of the Wild. TotK's decoder needs its Zstandard dictionaries, which Odyssey
  does not have.
- Coverage: every discovered shader is translated for the default bind state, not only TotK's startup packages.
- Runtime compatibility: the chain is this runtime's (dmsh_chain.h, v4); vertex rows in the *.variants indexes record
  depth_minus_w (Odyssey selects NVN's near-is-minus-w depth mode, and totk_rt_variant skips a vertex variant whose
  flag differs, so without it every seeded vertex shader would be ignored and translated again in play); and
  shader-keys.json is left out (this runtime's importer rejects any other file name, and with it the whole seed).
Output: an importable seed directory (default Odyssey_build/shader-seed-odyssey; the runtime imports
build/shader-seed). It contains game-derived data and stays in the ignored build folder.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys

GAME = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("ODYSSEY_WORK", GAME / "Odyssey_build")).resolve()   # ODYSSEY_WORK: another build folder
UPSTREAM = GAME / "third_party/totk-scripts/prepare_shader_seed.py"   # a copy of games/totk's (2026-10-22)
STAGE_NUMBER = {"vs": 0, "ps": 1, "cs": 5}   # NVN stage numbers in pso_manifest.tsv rows


def runtime_chain():
    text = (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text()
    return re.search(r'#define DMSH_CHAIN "([^"]+)"', text).group(1)


def load_upstream():
    sys.path.insert(0, str(UPSTREAM.parent))
    spec = importlib.util.spec_from_file_location("upstream_seed", UPSTREAM)
    seed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seed)
    seed.CHAIN = runtime_chain()
    scope = seed.shader_key_scope   # its chain default was bound to upstream's chain when the module loaded
    seed.shader_key_scope = lambda keys, version, source, chain=None: scope(keys, version, source, chain=seed.CHAIN)
    seed.decoder = lambda romfs, read, game="totk": (lambda raw: seed.yaz0(raw) if raw[:4] == seed.YAZ0 else raw)
    seed.startup_package = lambda name, game="totk": True   # every shader gets its default-state translation
    # Odyssey's shaders are only in these four folders (a scan of ObjectData, StageData, LocalizedData and EventData found
    # no BNSH / BAHS container; research, 22 October) plus the main module, so skip decompressing everything else.
    seed.GAMES["totk"]["folders"] = ("ShaderData/", "EffectData/", "LayoutData/", "SystemData/")
    # Odyssey's own shared list (none yet: an empty list), not the Tears of the Kingdom list the copy would look for.
    seed.GAMES["totk"]["list"] = GAME / "assets/shader_list.tsv"
    return seed


def finish(output: Path, chain: str):
    """Mark vertex variants with the depth mode, drop files the importer rejects, and re-hash the manifest."""
    vertex = fixed = 0
    for index in output.glob("*.variants"):
        rows = json.loads(index.read_text())
        for row in rows:
            refl = json.loads((output / f"{row['variant']}_b.json").read_text())
            if refl.get("stage") == "vs":
                vertex += 1
                if row.get("depth_minus_w") is not True:
                    row["depth_minus_w"] = True; fixed += 1
        index.write_text(json.dumps(rows, separators=(",", ":")) + "\n")
    (output / "shader-keys.json").unlink(missing_ok=True)
    allowed = re.compile(r"^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$")
    manifest = json.loads((output / "seed-manifest.json").read_text())
    if manifest.get("chain") != chain:
        raise SystemExit(f"seed chain {manifest.get('chain')} is not the runtime's {chain}")
    manifest["files"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(output.iterdir()) if allowed.match(p.name)}
    stray = [p.name for p in output.iterdir() if p.name != "seed-manifest.json" and not allowed.match(p.name)]
    if stray:
        raise SystemExit(f"unexpected files in the seed: {stray[:5]}")
    (output / "seed-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"vertex variants {vertex} (depth_minus_w added to {fixed}); {len(manifest['files'])} files in the manifest")


def salvage(seed, output: Path):
    """Publish a staging folder whose translation finished but whose last step failed (22 October: upstream's
    shader_key_scope kept its own chain). Rebuilds pso_manifest.tsv and seed-manifest.json from the files, finds the
    shaders that produced nothing, and translates those once more only to record why they fail."""
    import subprocess, struct, time
    staging = output.with_name(output.name + ".partial")
    romfs, translator, cross = BUILD / "extracted/local/romfs", BUILD / "build/shader-service/totk_xlate", BUILD / "build/spirv-cross/spirv-cross"
    index, shaders, startup, archives = seed.inventory(romfs, BUILD / "rewritten", "totk")
    done = {p.name[:-len(".variants")] for p in staging.glob("*.variants")}
    failed = sorted(k for k in shaders if k not in done)
    rows = []
    for p in sorted(staging.glob("*_b.json")):
        stage = json.loads(p.read_text())["stage"]
        variant = p.name[:-len(".json")]
        rows.append(f"S\t{variant}\t{STAGE_NUMBER[stage]}")
        if stage == "cs":
            rows.append(f"C\t{variant}")
    (staging / "pso_manifest.tsv").write_text("# Prepared from this app's local game data; " + seed.CHAIN + "\n" + "\n".join(rows) + "\n")
    errors = []
    probe = BUILD / "seed-failures"
    probe.mkdir(exist_ok=True)
    service = subprocess.Popen([str(translator), "--service", "--spirv-cross", str(cross), "--out", str(probe), "--chain", seed.CHAIN],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    for key in failed:
        stage, control, code, source = shaders[key]
        c1, length = struct.unpack_from("<II", control, 0x700)
        code_path = probe / (key + ".code")
        code_path.write_bytes(code[0 if stage == "cs" else 0x30:length])
        request = dict(variant=key + "-" + seed.SUFFIX, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                       depth_minus_w=True, **seed.ANSWERS)
        if stage == "cs":
            request["local"] = list(struct.unpack_from("<III", control, 0x748))
            shared = (struct.unpack_from("<I", control, 0x754)[0] + 15) // 16 * 16   # compute_shared.h: the kernel's shared memory,
            if 4096 < shared <= 32768:   # given only above the translator's 4 KiB default
                request["shared"] = shared
        service.stdin.write(json.dumps(request) + "\n"); service.stdin.flush()
        reply = json.loads(service.stdout.readline())
        errors.append(dict(key=key, stage=stage, source=source, ok=bool(reply.get("ok")), error=reply.get("error")))
    service.stdin.close(); service.wait()
    report = dict(schema=1, chain=seed.CHAIN, version="1.3.0", game="odyssey", source_romfs_sha256=index["romfs_sha256"],
                  shaders=len(done), discovered=len(shaders), startup_shaders=len(done), skipped=errors, archives=archives,
                  salvaged=time.strftime("%Y-%m-%d %H:%M"), files={})
    (staging / "seed-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    if output.exists():
        raise SystemExit(f"{output} already exists")
    staging.rename(output)
    print(f"salvaged: {len(done)} shaders of {len(shaders)} discovered; {len(failed)} produced nothing "
          f"({sum(e['ok'] for e in errors)} of them translate when retried)")


# Binding types Odyssey uses per slot (Applier's analysis of the runtime cache, 22 October): texture slot 9 is a cube map
# in every pixel and vertex shader that uses it, ps slots 6/7 are cubes (7 is sometimes 2d), ps 25 and cs 5 are 3d, and
# vs vertex input 11 is uint (sometimes float). With default answers the seed served 446 of 862 runtime variants; these
# rules (with the two alternates) reproduce 825 of 862 offline.
RULES = {"ps": [{"tex": {"9": "cube", "6": "cube", "7": "cube", "25": "3d"}}, {"tex": {"9": "cube", "6": "cube", "7": "2d", "25": "3d"}}],
         "vs": [{"tex": {"9": "cube"}, "attr": {"11": "uint"}}, {"tex": {"9": "cube"}, "attr": {"11": "float"}}],
         "cs": [{"tex": {"5": "3d"}}]}


def extend_with_rules(seed, output: Path):
    """Translate the rule variants of every seed shader that uses a ruled slot (and of the shaders whose default
    translation failed, with the full rule), add them to the shader's .variants (deps that duplicate an existing row are
    dropped), then re-run finish()."""
    import concurrent.futures, struct, subprocess, threading
    translator, cross = BUILD / "build/shader-service/totk_xlate", BUILD / "build/spirv-cross/spirv-cross"
    _, shaders, _, _ = seed.inventory(BUILD / "extracted/local/romfs", BUILD / "rewritten", "totk")
    jobs = []
    for key, (stage, control, code, source) in shaders.items():
        index = output / f"{key}.variants"
        rows = json.loads(index.read_text()) if index.exists() else None
        used = None
        if rows:
            deps = rows[0]["deps"]
            used = {"tex": set(deps.get("tex", {})), "attr": set(deps.get("attr", {}))}
        for rule in RULES.get(stage, []):
            answers = {kind: dict(seed.ANSWERS[kind]) for kind in seed.ANSWERS}
            for kind, slots in rule.items():
                for slot, kind_type in slots.items():
                    if used is None or slot in used[kind]:
                        answers[kind][slot] = kind_type
            if answers != seed.ANSWERS:
                jobs.append((key, stage, answers))
    by_key = {}
    for job in jobs:
        by_key.setdefault(job[0], []).append(job)
    print(f"rule jobs: {len(jobs)} for {len(by_key)} shaders", flush=True)
    pending, lock, added, failed_keys, rescued = iter(by_key.items()), threading.Lock(), [0], set(), set()

    def worker():
        service = subprocess.Popen([str(translator), "--service", "--spirv-cross", str(cross), "--out", str(output),
                                    "--chain", seed.CHAIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            while True:
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, mine = item
                index = output / f"{key}.variants"
                rows = json.loads(index.read_text()) if index.exists() else []
                had = bool(rows)
                for _, stage, answers in mine:
                    _, control, code, _ = shaders[key]
                    variant = key + "-" + seed.suffix(answers)
                    c1, length = struct.unpack_from("<II", control, 0x700)
                    code_path = output / (variant + ".code")
                    code_path.write_bytes(code[0 if stage == "cs" else 0x30:length])
                    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                                   depth_minus_w=True, **answers)
                    if stage == "cs":
                        request["local"] = list(struct.unpack_from("<III", control, 0x748))
                        shared = (struct.unpack_from("<I", control, 0x754)[0] + 15) // 16 * 16   # compute_shared.h: the kernel's shared memory,
                        if 4096 < shared <= 32768:   # given only above the translator's 4 KiB default
                            request["shared"] = shared
                    service.stdin.write(json.dumps(request) + "\n"); service.stdin.flush()
                    reply = json.loads(service.stdout.readline())
                    code_path.unlink(missing_ok=True)
                    if not reply.get("ok"):
                        for name in (variant + "_b.metal", variant + "_b.json"):
                            (output / name).unlink(missing_ok=True)
                        with lock:
                            failed_keys.add(key)
                        continue
                    deps = json.loads((output / f"{variant}_b.json").read_text())["deps"]
                    if any(r["deps"] == deps for r in rows):
                        if all(r["variant"] != variant for r in rows):
                            for name in (variant + "_b.metal", variant + "_b.json"):
                                (output / name).unlink(missing_ok=True)
                        continue
                    row = dict(variant=variant, deps=deps)
                    if stage == "vs":
                        row["depth_minus_w"] = True
                    rows.append(row)
                    with lock:
                        added[0] += 1
                        if not had:
                            rescued.add(key)
                if rows:
                    index.write_text(json.dumps(rows, separators=(",", ":")) + "\n")
        finally:
            service.stdin.close(); service.wait()

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for future in [pool.submit(worker) for _ in range(8)]:
            future.result()
    for leftover in [*output.glob("*.spv"), *output.glob("*.res.json")]:   # translator intermediates (upstream deletes them too)
        leftover.unlink()
    manifest = json.loads((output / "seed-manifest.json").read_text())
    manifest["skipped"] = [s for s in manifest.get("skipped", []) if s["key"] not in rescued]
    manifest["shaders"] = sum(1 for _ in output.glob("*.variants"))
    manifest["rule_variants_added"] = added[0]
    (output / "seed-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    rows = []
    for p in sorted(output.glob("*_b.json")):
        stage = json.loads(p.read_text())["stage"]; variant = p.name[:-len(".json")]
        rows.append(f"S\t{variant}\t{STAGE_NUMBER[stage]}")
        if stage == "cs":
            rows.append(f"C\t{variant}")
    (output / "pso_manifest.tsv").write_text("# Prepared from this app's local game data; " + seed.CHAIN + "\n" + "\n".join(rows) + "\n")
    print(f"rule variants added: {added[0]}; previously failed shaders now translated: {len(rescued)}; "
          f"shaders with a failed rule job: {len(failed_keys)}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--output", type=Path, default=BUILD / "shader-seed-odyssey")
    p.add_argument("--finish-only", action="store_true", help="only re-run the Odyssey fix-ups on an existing seed")
    p.add_argument("--salvage", action="store_true", help="publish <output>.partial after a failed last step, then finish")
    p.add_argument("--rules", action="store_true", help="add the per-slot rule variants to an existing seed, then finish")
    a = p.parse_args()
    seed = load_upstream()
    if a.rules:
        extend_with_rules(seed, a.output)
    elif a.salvage:
        salvage(seed, a.output)
    elif not a.finish_only:
        seed.prepare(BUILD / "extracted/local/romfs", a.output, BUILD / "build/shader-service/totk_xlate",
                     BUILD / "build/spirv-cross/spirv-cross", "1.3.0", modules=BUILD / "rewritten", game="totk")
    finish(a.output, seed.CHAIN)


if __name__ == "__main__":
    main()
