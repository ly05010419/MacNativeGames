#!/usr/bin/env python3
"""Translate every shader of the game's own files ahead of play, into a folder in the runtime's cache format.

monolith_shaders.py finds the shaders (the merged RomFS, the add-ons, main) and keys them as the runtime does. Each vertex,
pixel and compute shader is translated once for the bind state the runtime's own ahead-of-draw translation asks with
(totk_shader_rt.m totk_rt_prefetch: every texture slot "buffer", which leaves the kind to the instructions; float inputs and
outputs), so the variant has the name the runtime gives it (12,826 of the 12,952 variants met in play), with the request the runtime would send (totk_shader_rt.m rt_request: the code from +0x30, c1, a compute shader's group size
and shared memory, early fragment tests, depth_minus_w as this game selects NEAR_IS_MINUS_W), through the same translator
service. The runtime picks a variant by its deps (totk_shader_rt.m deps_match), so a default translation serves every draw
whose bound state agrees with what the translator asked; 91% of the shaders met in play on the player's save had such a
variant (2026-11-03). Geometry shaders are not taken: the runtime translates them as compute in milliseconds (-vsc-/-gsc-).

Shaders whose translation input is the same (translation_input) are translated once; the others get a .variants index naming
that variant, as games/totk/scripts/prepare_shader_seed.py does. A variant already in the folder is not translated again.
The folder is then one of prepare_shader_seed.py's caches (make shader-seed). It holds game-derived data: work folder only.
usage: translate_shaders.py --out <work>/build/shader-prepared --translator <totk_xlate> --spirv-cross <spirv-cross>
       [--main <main image>] <extracted folder> [<extracted folder> ...]
"""
import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from monolith_shaders import census, ctl_key_len  # noqa: E402

CHAIN_H = Path(__file__).resolve().parents[1] / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
ANSWERS = dict(attr={}, fout={}, img={}, tex={str(r): 'buffer' for r in range(32)})   # totk_shader_rt.m totk_rt_prefetch
STAGE_NUM = {'vs': 0, 'ps': 1, 'cs': 5}


def chain():
    m = re.search(r'#define DMSH_CHAIN "([^"]+)"', CHAIN_H.read_text())
    if not m:
        sys.exit(f'no DMSH_CHAIN in {CHAIN_H}')
    return m.group(1)


def ns_json(v):
    """NSJSONSerialization with NSJSONWritingSortedKeys, compact: it orders keys like "2" before "10" (numeric
    comparison), unlike sort_keys."""
    if isinstance(v, dict):
        order = sorted(v, key=lambda k: (0, int(k), '') if k.isdigit() else (1, 0, k))
        return '{' + ','.join(json.dumps(k) + ':' + ns_json(v[k]) for k in order) + '}'
    return json.dumps(v, separators=(',', ':'))


def variant_name(key, answers=ANSWERS):
    """totk_shader_rt.m: <key>-<the first 6 bytes of the SHA-256 of json_string(answers)>."""
    return key + '-' + hashlib.sha256(ns_json(answers).encode()).hexdigest()[:12]


def early_fragment_tests(stage, control):
    """totk_shader_rt.m rt_request: a pixel shader whose control section is the full 0x880 bytes, flag +0x718 bit 0."""
    return stage == 'ps' and ctl_key_len(control) == 0x880 and bool(control[0x718] & 1)


def request(variant, stage, control, code, code_path):
    """totk_shader_rt.m rt_request for the default answers."""
    c1o, c1t = struct.unpack_from('<II', control, 0x700)
    skip = 0 if stage == 'cs' else 0x30
    if not c1t or c1t > len(code) or c1o > c1t or (stage != 'cs' and c1o < 0x30):
        raise ValueError(f'{variant}: code area {c1t:#x} / c1 {c1o:#x}')
    code_path.write_bytes(code[skip:c1t])
    rq = dict(variant=variant, stage=stage, code=str(code_path), c1=code[c1o:c1t].hex(), **ANSWERS)
    if stage == 'cs':
        local = list(struct.unpack_from('<3I', control, 0x748))
        if not all(local) or local[0] * local[1] * local[2] > 1024:
            raise ValueError(f'{variant}: thread group {local}')
        rq['local'] = local
        shared = struct.unpack_from('<I', control, 0x754)[0]
        if shared:
            rq['shared'] = shared
    if early_fragment_tests(stage, control):
        rq['early_fragment_tests'] = True
    rq['depth_minus_w'] = True
    return rq


def translation_input(stage, control, code):
    """Everything a translation reads of a shader (request() without the variant name)."""
    c1o, c1t = struct.unpack_from('<II', control, 0x700)
    return hashlib.sha256(stage.encode() + struct.pack('<II', c1o, c1t) + code[0 if stage == 'cs' else 0x30:c1t] +
                          (control[0x748:0x758] if stage == 'cs' else b'') +
                          (b'E' if early_fragment_tests(stage, control) else b'')).hexdigest()


def done(out, variant, expected_chain, stage):
    try:
        if not (out / f'{variant}_b.metal').stat().st_size:
            return False
        reflection = json.loads((out / f'{variant}_b.json').read_text())
        return reflection.get('chain') == expected_chain and reflection.get('stage') == stage
    except (OSError, ValueError, AttributeError):
        return False


def prepare_output(out, translator, spirv_cross, name_chain):
    """Keep old game-derived results, but never reuse them with different tools or request code."""
    def digest(path):
        h = hashlib.sha256()
        with Path(path).open('rb') as f:
            for block in iter(lambda: f.read(1 << 20), b''):
                h.update(block)
        return h.hexdigest()
    identity = dict(schema=1, chain=name_chain,
                    translator=digest(translator), spirv_cross=digest(spirv_cross),
                    requests=digest(__file__), containers=digest(Path(__file__).with_name('monolith_shaders.py')))
    marker = out / 'translation-build.json'
    try:
        previous = json.loads(marker.read_text())
    except (OSError, ValueError):
        previous = None
    if out.exists() and any(out.iterdir()) and previous != identity:
        retired = out.with_name(out.name + '.stale-' + str(time.time_ns()))
        out.rename(retired)
        print(f'kept incompatible translations in {retired}', flush=True)
    out.mkdir(parents=True, exist_ok=True)
    temporary = out / 'translation-build.json.partial'
    temporary.write_text(json.dumps(identity, sort_keys=True, indent=2) + '\n')
    temporary.replace(marker)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folders', type=Path, nargs='+', help='extracted folders holding romfs/ (the game, each add-on)')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--spirv-cross', type=Path, required=True)
    ap.add_argument('--main', type=Path, help="main's decompressed image (rewritten/<version>/main.patched.bin)")
    ap.add_argument('--workers', type=int, default=min(8, os.cpu_count() or 4))
    a = ap.parse_args()
    out = a.out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    output_lock = out.parent.joinpath('.' + out.name + '.translation.lock').open('a')
    fcntl.flock(output_lock.fileno(), fcntl.LOCK_EX)
    name_chain = chain()
    prepare_output(out, a.translator, a.spirv_cross, name_chain)
    started = time.monotonic()
    found, damaged = census(a.folders, a.main)
    shaders = {k: v for k, v in found.items() if v[0] in STAGE_NUM}
    print(f'{len(found)} shaders found ({len(damaged)} damaged entries skipped), {len(shaders)} vertex / pixel / compute; '
          f'{time.monotonic() - started:.0f}s', flush=True)
    same = {}
    for key in sorted(shaders):
        same.setdefault(translation_input(*shaders[key]), []).append(key)
    firsts = [keys[0] for keys in same.values()]
    todo = [k for k in firsts if not done(out, variant_name(k), name_chain, shaders[k][0])]
    print(f'{len(firsts)} distinct translations, {len(todo)} to translate', flush=True)
    lock = threading.Lock()
    pending = iter(todo)
    count = [0, 0]   # translated, failed
    failed = []
    stop = threading.Event()

    def worker():
        process = subprocess.Popen([str(a.translator), '--service', '--spirv-cross', str(a.spirv_cross), '--out', str(out),
                                    '--chain', name_chain], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            while not stop.is_set():
                with lock:
                    key = next(pending, None)
                if key is None:
                    return
                stage, control, code = shaders[key]
                variant = variant_name(key)
                for ext in ('metal', 'json'):
                    (out / f'{variant}_b.{ext}').unlink(missing_ok=True)
                code_path = out / f'{variant}.code'
                try:
                    rq = request(variant, stage, control, code, code_path)
                except ValueError as e:
                    reply = dict(ok=False, error=str(e))
                else:
                    process.stdin.write(json.dumps(rq) + '\n')
                    process.stdin.flush()
                    line = process.stdout.readline()
                    if not line:
                        raise RuntimeError('the translator service exited')
                    reply = json.loads(line)
                code_path.unlink(missing_ok=True)
                for end in ('.spv', '.res.json'):   # the translator's intermediates: the seed takes only _b.metal / _b.json
                    (out / (variant + end)).unlink(missing_ok=True)
                ok = reply.get('ok') and done(out, variant, name_chain, stage)
                if ok:
                    refl = json.loads((out / f'{variant}_b.json').read_text())
                    if refl.get('chain') != name_chain or refl.get('stage') != stage:
                        raise RuntimeError(f'{variant}: reflection chain / stage mismatch')
                else:
                    for ext in ('metal', 'json'):
                        (out / f'{variant}_b.{ext}').unlink(missing_ok=True)
                with lock:
                    count[0 if ok else 1] += 1
                    if not ok:
                        failed.append(f'{variant} {stage}: {reply.get("error")}')
                    n = count[0] + count[1]
                    if n % 500 == 0:
                        print(f'translated {count[0]}, failed {count[1]} of {len(todo)}; {time.monotonic() - started:.0f}s', flush=True)
        except BaseException:
            stop.set()
            raise
        finally:
            process.stdin.close()
            process.wait()

    workers = max(1, min(a.workers, len(todo)))
    if todo:
        with concurrent.futures.ThreadPoolExecutor(workers) as pool:
            for future in [pool.submit(worker) for _ in range(workers)]:
                future.result()
    indexes = 0
    for keys in same.values():
        variant = variant_name(keys[0])
        if not done(out, variant, name_chain, shaders[keys[0]][0]):
            continue
        deps = json.loads((out / f'{variant}_b.json').read_text()).get('deps', {})
        row = json.dumps([dict(deps=deps, variant=variant)], sort_keys=True, separators=(',', ':'))
        for key in keys:
            (out / f'{key}.variants').write_text(row)
            indexes += 1
    (out / 'failed.txt').write_text('\n'.join(failed) + ('\n' if failed else ''))
    print(json.dumps(dict(out=str(out), chain=name_chain, shaders=indexes, translated=count[0], failed=count[1],
                          seconds=round(time.monotonic() - started))))


if __name__ == '__main__':
    main()
