#!/usr/bin/env python3
"""Prepare Animal Crossing's shader seed from the verified, prepared player data (reported in play, PR #1305: frame drops).

Without it, every shader the game meets for the first time is translated and compiled on the submit thread: in a
10-minute session of 3.0.1, 25 of the 26 frames over 50 ms were that (17.9 of 19.2 s lost, the worst 6.1 s for 61
shader libraries and 58 pipelines). The seed holds every shader the game's files carry, translated for the default
bind state: the app imports it into the shader cache at start (totk_shader_seed.h) and the start-up warm-up compiles it
(nvn_mtl.m warm_start, shader-seed/pso_manifest.tsv) before play.

Where the shaders are (a scan of 3.0.1): the uber shader and the other Shader/ archives (Yaz0 BFSHA), Effect/ and
Layout/ resources, the few Model/ files with their own BNSH, System/, and the main module. Every BNSH found is read as
NVN does: the shader key is SHA-256 of the control section (its first min(word +0x20, 0x880) bytes) and the code up to
the control's code size, as the runtime keys it (nvn_mtl.m ctl_key_len); Animal Crossing binds these sections
unmodified (405 of the 474 shaders of a player's sessions had a key found here). Vertex shaders are translated with
depth_minus_w (the game selects NVN's near-is-minus-w depth mode; every vertex variant of a player's cache has it).
A shader whose translation names shadow-compare slots (compare_slots) is translated a second time with depth textures
bound there, as games/totk's seed does: the game binds its shadow maps there, and 90 of the 475 variants of a player's
sessions were those.

The requests are the runtime's own (totk_shader_rt.m totk_rt_variant): the code after the 0x30-byte NVN header (compute
keeps it), c1 the constant area, the bind-state answers and, for compute, the thread group size at control+0x748; the
variant is named as the runtime names one. The output is game-derived data: it stays in the ignored work folder and in
the app, never in the repository. Rerunning with the same inputs, translator and tool reuses the published seed.
Adapted from games/totk/scripts/prepare_shader_seed.py (container parsing, the seed manifest) for this game's data
layout; no other game's files are read.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from compression import zstd

HERE = Path(__file__).resolve()
GAME = HERE.parents[1]
CHAIN = re.search(r'#define DMSH_CHAIN "([^"]+)"', (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text()).group(1)
ANSWERS = dict(tex={}, img={}, attr={}, fout={})   # the default bind state
STAGES = {"vs": 0, "ps": 1, "cs": 5}
FOLDERS = ("Shader/", "Effect/", "Layout/", "System/", "Model/", "Swkbd/")
YAZ0, ZSTD, BNSH = b"Yaz0", b"\x28\xb5\x2f\xfd", b"BNSH\0\0\0\0"
SEED_FILE = re.compile(r"^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError("shader container range outside file")
    return data[offset:offset + size]


def control_key_len(control):
    """How many bytes of a control section the runtime hashes into the shader key (nvn_mtl.m ctl_key_len)."""
    size = struct.unpack_from("<I", control, 0x20)[0] if len(control) >= 0x24 else 0
    return size if 0x7cc <= size < 0x880 else 0x880


def yaz0(data):
    """Yaz0: a code byte, then 8 items, a literal byte or a back reference (distance, length)."""
    size = struct.unpack_from(">I", data, 4)[0]
    if size > 512 << 20:
        raise ValueError("unbounded Yaz0 file")
    out, o, i = bytearray(size), 0, 16
    try:
        while o < size:
            code = data[i]; i += 1
            for bit in (0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1):
                if o >= size:
                    break
                if code & bit:
                    out[o] = data[i]; o += 1; i += 1
                    continue
                first = data[i]; dist = ((first & 0xF) << 8 | data[i + 1]) + 1; length = first >> 4; i += 2
                if length:
                    length += 2
                else:
                    length = data[i] + 0x12; i += 1
                length = min(length, size - o)
                src = o - dist
                if src < 0:
                    raise ValueError("Yaz0 back reference before the start")
                if dist >= length:
                    out[o:o + length] = out[src:src + length]
                else:
                    for k in range(length):
                        out[o + k] = out[src + k]
                o += length
    except IndexError:
        raise ValueError("truncated Yaz0 file") from None
    return bytes(out)


def unpack(raw):
    if raw[:4] == YAZ0:
        return yaz0(raw)
    if raw[:4] == ZSTD:
        return zstd.decompress(raw)
    return raw


def sarc_members(data):
    """The members of a SARC archive (no names needed: only their bytes are scanned)."""
    endian = "<" if data[6:8] == b"\xff\xfe" else ">"
    header, size, start = struct.unpack_from(endian + "HxxII", data, 4)
    if size != len(data) or checked(data, header, 4) != b"SFAT":
        raise ValueError("invalid SARC header")
    count = struct.unpack_from(endian + "H", data, header + 6)[0]
    for i in range(count):
        _, _, lo, hi = struct.unpack_from(endian + "4I", checked(data, header + 12 + i * 16, 16))
        if hi < lo:
            raise ValueError("invalid SARC member")
        yield checked(data, start + lo, hi - lo)


def bnsh_shaders(data):
    """(key, stage, control, code) of every program of one BNSH file; offsets are relative to it."""
    if checked(data, 12, 2) != b"\xff\xfe" or checked(data, 96, 4) != b"grsc":
        raise ValueError("unsupported BNSH header")
    data = checked(data, 0, struct.unpack_from("<I", data, 28)[0])
    count, variations = struct.unpack_from("<IQ", data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from("<Q", data, variations + i * 64 + 16)[0]
        checked(data, program, 56)
        for stage, relative in (("vs", 8), ("ps", 40), ("cs", 48)):
            offset = struct.unpack_from("<Q", data, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from("<QQII", checked(data, offset + 8, 24))
            control, code = checked(data, control_off, control_size), checked(data, code_off, code_size)
            if len(control) < 0x7cc:   # the SDK utility's older, shorter control layout: not one the game binds
                continue
            key_len = control_key_len(control)
            if len(control) < key_len:
                continue
            control = control[:key_len]
            c1, length = struct.unpack_from("<II", control, 0x700)
            if struct.unpack_from("<I", control, 0x714)[0] != STAGES[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError("invalid NVN shader bounds or stage")
            code = code[:length]
            yield sha256(control + code), stage, control, code


def scan(data, found, source, depth=0):
    """Every BNSH in data (and in the members of a SARC in it) into found: key -> (stage, control, code, source)."""
    if data[:4] == b"SARC" and depth < 2:
        try:
            for member in sarc_members(data):
                scan(unpack(member), found, source, depth + 1)
            return
        except ValueError:
            pass
    pos = 0
    while (pos := data.find(BNSH, pos)) >= 0:
        try:
            for key, stage, control, code in bnsh_shaders(data[pos:]):
                found.setdefault(key, (stage, control, code, source))
        except (ValueError, struct.error):
            pass   # "BNSH" bytes that are not a shader file
        pos += 8


def inventory(prepared):
    """The shaders of the prepared RomFS and modules, and the RomFS identity they come from."""
    romfs = prepared / "update/romfs"
    index = json.loads((romfs / "index.json").read_text())
    found = {}
    with (romfs / "romfs.bin").open("rb") as blob:
        for f in index["files"]:
            if not f["path"].startswith(FOLDERS):
                continue
            blob.seek(f["offset"]); raw = blob.read(f["size"])
            if f["path"].startswith("Model/") and BNSH not in raw[:16] and raw[:4] not in (ZSTD, YAZ0):
                continue
            try:
                data = unpack(raw)
            except Exception as error:   # noqa: BLE001 - a file that is not a container is skipped, not fatal
                print(f"skipped {f['path']}: {error}", file=sys.stderr)
                continue
            scan(data, found, f["path"].split("/")[0])
    for module in sorted((prepared / "update/rewritten").glob("*/*.patched.bin")):
        scan(module.read_bytes(), found, module.stem.split(".")[0])
    return found, index["romfs_sha256"]


def variant_name(key, stage, answers=ANSWERS):
    """The runtime's name for a variant (totk_rt_variant: the answers, with depth_minus_w for vertex, as sorted compact
    JSON)."""
    named = dict(answers)
    if stage == "vs":
        named["depth_minus_w"] = True
    return key + "-" + sha256(json.dumps(named, sort_keys=True, separators=(",", ":")).encode())[:12]


def depth_answers(slots):
    """The bind state with depth textures in the shadow-compare slots (games/totk answers_for of "*_depth")."""
    return dict(ANSWERS, tex={str(slot): "2d_depth" for slot in slots})


def request(variant, stage, control, code, code_path, answers=ANSWERS):
    c1, length = struct.unpack_from("<II", control, 0x700)
    code_path.write_bytes(code[0 if stage == "cs" else 0x30:length])
    rq = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
              depth_minus_w=stage == "vs", **answers)
    if stage == "cs":
        rq["local"] = list(struct.unpack_from("<III", control, 0x748))
    return rq


def identity(romfs_sha256, translator):
    return dict(chain=CHAIN, source_romfs_sha256=romfs_sha256, translator_sha256=sha256(translator.read_bytes()),
                tool_sha256=sha256(HERE.read_bytes()))


def reusable(output, ident):
    try:
        manifest = json.loads((output / "seed-manifest.json").read_text())
    except (OSError, ValueError):
        return False
    return all(manifest.get(k) == v for k, v in ident.items()) and all((output / n).is_file() for n in manifest.get("files", {}))


def translate(shaders, work, staging, translator, jobs):
    """Translate work, a list of (key, answers), with jobs translator services. Returns the variants made per key
    (variant, stage, deps, compare_slots) and the failures."""
    pending, lock, made, failed, done = iter(work), threading.Lock(), {}, [], [0]
    started = time.monotonic()

    def worker():
        service = subprocess.Popen([str(translator), "--service", "--spirv-cross", str(translator.parent / "spirv-cross"),
                                    "--out", str(staging), "--chain", CHAIN],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            while True:
                with lock:
                    job = next(pending, None)
                if job is None:
                    return
                key, answers = job
                stage, control, code, source = shaders[key]
                variant = variant_name(key, stage, answers)
                code_path = staging / (variant + ".code")
                service.stdin.write(json.dumps(request(variant, stage, control, code, code_path, answers)) + "\n"); service.stdin.flush()
                line = service.stdout.readline()
                if not line:
                    raise RuntimeError("the translator service exited")
                reply = json.loads(line)
                code_path.unlink(missing_ok=True)
                refl = None
                if reply.get("ok"):
                    refl = json.loads((staging / (variant + "_b.json")).read_text())
                    if refl.get("chain") != CHAIN or refl.get("stage") != stage:
                        raise RuntimeError(f"{variant}: translation identity mismatch")
                with lock:
                    if refl is None:
                        failed.append(dict(key=key, variant=variant, stage=stage, source=source, error=str(reply.get("error"))[:300]))
                    else:
                        made.setdefault(key, []).append((variant, stage, refl.get("deps") or {}, refl.get("compare_slots") or []))
                    done[0] += 1
                    if done[0] % 1000 == 0:
                        print(f"  {done[0]} / {len(work)} translations, {time.monotonic() - started:.0f} s", flush=True)
        finally:
            service.stdin.close(); service.wait()

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for future in [pool.submit(worker) for _ in range(jobs)]:
            future.result()
    return made, failed


def prepare(prepared, translator, output, jobs, version):
    started = time.monotonic()
    shaders, romfs_sha256 = inventory(prepared)
    by_source = {}
    for stage, _, _, source in shaders.values():
        by_source[source] = by_source.get(source, 0) + 1
    print(f"found {len(shaders)} shaders ({', '.join(f'{k} {v}' for k, v in sorted(by_source.items()))}) in {time.monotonic() - started:.0f} s", flush=True)
    ident = identity(romfs_sha256, translator)
    if reusable(output, ident):
        print(f"{output}: already prepared from these inputs; kept")
        return
    staging = output.with_name(output.name + ".partial")
    shutil.rmtree(staging, ignore_errors=True); staging.mkdir(parents=True)
    made, failed = translate(shaders, [(key, ANSWERS) for key in sorted(shaders)], staging, translator, jobs)
    depth = [(key, depth_answers(made[key][0][3])) for key in sorted(made) if made[key][0][3]]
    more, failed_depth = translate(shaders, depth, staging, translator, jobs)
    failed += failed_depth
    for key, variants in more.items():
        made[key] += variants
    for leftover in [*staging.glob("*.spv"), *staging.glob("*.res.json"), *staging.glob("*.code")]:   # translator intermediates
        leftover.unlink()
    rows, extra = [], 0
    for key, variants in sorted(made.items()):
        records, seen = [], []
        for variant, stage, deps, _ in variants:
            if deps in seen:   # the depth translation came out the same: one variant serves both
                for name in (variant + "_b.metal", variant + "_b.json"):
                    (staging / name).unlink(missing_ok=True)
                continue
            seen.append(deps); extra += len(seen) > 1
            record = dict(variant=variant, deps=deps)
            if stage == "vs":
                record["depth_minus_w"] = True
            records.append(record)
            rows.append(f"S\t{variant}_b\t{STAGES[stage]}")
            if stage == "cs":
                rows.append(f"C\t{variant}_b")
        (staging / f"{key}.variants").write_text(json.dumps(records, sort_keys=True, separators=(",", ":")) + "\n")
    (staging / "pso_manifest.tsv").write_text(f"# Prepared from this app's local game data; {CHAIN}\n" + "\n".join(rows) + "\n")
    stray = sorted(p.name for p in staging.iterdir() if not SEED_FILE.match(p.name))
    if stray:
        raise SystemExit(f"unexpected files in the seed: {stray[:5]}")
    files = {p.name: sha256(p.read_bytes()) for p in sorted(staging.iterdir())}
    manifest = dict(schema=1, **ident, game="acnh", version=version, shaders=len(made), discovered=len(shaders), depth_variants=extra,
                    sources=by_source, skipped=failed, files=files, prepared=time.strftime("%Y-%m-%d %H:%M"))
    (staging / "seed-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.rmtree(output, ignore_errors=True)
    staging.rename(output)
    print(f"{output}: {len(made)} shaders translated ({extra} with a shadow-map variant too), {len(failed)} failed, {len(files)} files, "
          f"{time.monotonic() - started:.0f} s", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--prepared", type=Path, required=True, help="the prepare.py work folder (update/romfs, update/rewritten)")
    p.add_argument("--translator", type=Path, required=True, help="the pinned totk_xlate (the native work's build/shader-service)")
    p.add_argument("--output", type=Path, required=True, help="the seed folder (package_native_app.py bundles <work>/build/shader-seed)")
    p.add_argument("--version", help="the label in the seed's manifest (default: the prepared folder's game version)")
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    a = p.parse_args()
    if not a.translator.is_file():
        raise SystemExit(f"{a.translator}: no translator (run build_host.py first)")
    if a.version is None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
        from games.acnh.prepare import prepared_profile
        a.version = prepared_profile(a.prepared.resolve())["display_version"]
    prepare(a.prepared.resolve(), a.translator.resolve(), a.output.resolve(), a.jobs, a.version)


if __name__ == "__main__":
    main()
