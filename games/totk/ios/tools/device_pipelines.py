#!/usr/bin/env python3
"""The device's pipeline archive, compiled on the Mac while packaging (2026-10-29).

build_ipa.py first runs the Mac game binary's pipeline-archive mode, which makes every pipeline of the shader lists from
the device seed's metallibs, one Metal archive file each, as the device's own first start does (nvn_mtl.m
mtl_pipeline_archive_tool). This script turns each of those files into the code of one device GPU with metal-tt and puts
the results into the seed, flat (a pushed seed is one stream of files: install_device.py write_pack):

  pipelines-<arch>-<name>.metalarchive  the GPU code of the pipelines of one file
  pipelines-<arch>.tsv                  "<key>\t<name>" for every key of those files
  pipelines-<arch>.json                 schema, chain, arch: the runtime takes them only when all three match
                                        (totk_pipeline_archive.h par_load_prebuilt)

--variants <folder>: first, the patched variants (border colours, tile reads, ...) the runtime compiles from source at
run time, recorded by a first run of that mode (nvn_mtl.m variant_record: <SHA-256 of the source>.metal), are compiled
for the device into the seed as variant-<SHA-256>.metallib, with the seed's own options (compile_seed.py METAL_FLAGS).
The device loads them instead of compiling them (nvn_mtl.m variant_library_seed), and a second run of the mode makes
their pipelines from them, so these can be compiled for the GPU too (BotW 1.6.0: 1,111 pipelines, 58 s on an iPhone
14's first start before). A pipeline whose functions still come from a library the seed does not have is left out;
the device builds it on its first start. Results are kept by the content of their pipeline script or source and the
compiler (<cache>/<arch>/, <cache>/variant-libs/), so a build whose lists and shaders did not change compiles nothing again.
Measured: BotW 1.6.0, 13,760 pipelines; metal-tt takes about 0.4 s per pipeline on an M4, run on every core.
"""
import argparse, concurrent.futures, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
from compile_seed import METAL_FLAGS

SCHEMA = 5   # totk_pipeline_archive.h PAR_SCHEMA

# The GPU of the iPhones and iPads Metal 4 runs on (A14 / M1 and later), by productType (devicectl). Only used to choose
# what to compile; the runtime checks the architecture itself (MTLDevice.architecture), so a wrong entry costs the
# device its own first fill, never a wrong pipeline.
GPU_BY_PRODUCT = {
    **{f'iPhone13,{i}': 'applegpu_g13p' for i in range(1, 5)},                          # iPhone 12 (A14)
    **{f'iPhone14,{i}': 'applegpu_g14p' for i in range(2, 9)},                          # iPhone 13, SE 3, 14 (A15)
    **{f'iPhone15,{i}': 'applegpu_g15p' for i in range(2, 6)},                          # iPhone 14 Pro, 15 (A16)
    'iPhone16,1': 'applegpu_g16p', 'iPhone16,2': 'applegpu_g16p',                         # iPhone 15 Pro (A17 Pro)
    **{f'iPhone17,{i}': 'applegpu_g17p' for i in range(1, 6)},                          # iPhone 16 (A18)
    'iPad13,1': 'applegpu_g13p', 'iPad13,2': 'applegpu_g13p',                             # iPad Air 4 (A14)
    'iPad13,18': 'applegpu_g13p', 'iPad13,19': 'applegpu_g13p',                           # iPad 10 (A14)
    **{f'iPad13,{i}': 'applegpu_g13g' for i in (4, 5, 6, 7, 8, 9, 10, 11, 16, 17)},     # iPad Pro / Air (M1)
    'iPad14,1': 'applegpu_g14p', 'iPad14,2': 'applegpu_g14p',                             # iPad mini 6 (A15)
    **{f'iPad14,{i}': 'applegpu_g14g' for i in (3, 4, 5, 6, 8, 9, 10, 11)},             # iPad Pro / Air (M2)
    'iPad15,7': 'applegpu_g15p', 'iPad15,8': 'applegpu_g15p',                             # iPad (A16)
    'iPad16,1': 'applegpu_g16p', 'iPad16,2': 'applegpu_g16p',                             # iPad mini 7 (A17 Pro)
    **{f'iPad16,{i}': 'applegpu_g16g' for i in (3, 4, 5, 6)},                           # iPad Pro (M4)
}


def connected_gpus(env):
    """The GPU architectures of the iPhones / iPads on the cable (devicectl), with their names; unknown models are named
    with None."""
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / 'devices.json'
        r = subprocess.run(['xcrun', 'devicectl', 'list', 'devices', '--json-output', str(out)], env=env, capture_output=True)
        if r.returncode or not out.is_file(): return []
        devices = json.loads(out.read_text()).get('result', {}).get('devices', [])
    found = []
    for dev in devices:
        hw = dev.get('hardwareProperties', {})
        if hw.get('platform') != 'iOS' or dev.get('connectionProperties', {}).get('transportType') != 'wired': continue
        found.append((dev.get('deviceProperties', {}).get('name') or hw.get('marketingName', '?'), hw.get('productType', '?'),
                      GPU_BY_PRODUCT.get(hw.get('productType'))))
    return found


def tool(sdk, name, env):
    return subprocess.run(['xcrun', '-sdk', sdk, '-f', name], env=env, check=True, capture_output=True, text=True).stdout.strip()


def script_of(archive, sdk, scripts, env):
    """The pipeline script (.mtlp-json) metal-source extracts from a Mac archive file, kept by the file's hash."""
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    kept = scripts / f'{digest}.mtlp-json'
    if not kept.is_file():
        with tempfile.TemporaryDirectory(dir=scripts) as d:
            subprocess.run(['xcrun', '-sdk', sdk, 'metal-source', '--extract=metal-scripts', '--flatbuffers=json', '-o', d, str(archive)],
                           env=env, capture_output=True)
            found = sorted(Path(d).rglob('*.mtlp-json'))
            if not found: return None
            os.replace(found[0], kept)
    return json.loads(kept.read_text())


def place(src, dst):
    if subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode: shutil.copyfile(src, dst)


def variants(folder, seed, cache, sdk, env):
    """The recorded variant sources compiled into the seed as variant-<SHA-256>.metallib (see the top); the last build's
    that are not recorded now are removed. Returns the report."""
    for old in seed.glob('variant-*'): old.unlink()
    sources = [p for p in sorted(folder.glob('*.metal')) if len(p.stem) == 64 and hashlib.sha256(p.read_bytes()).hexdigest() == p.stem] if folder.is_dir() else []
    compiler = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], env=env, check=True, capture_output=True, text=True).stdout
    kept_dir = cache / 'variant-libs'; kept_dir.mkdir(parents=True, exist_ok=True)
    def compile_one(src):
        kept = kept_dir / (hashlib.sha256('\0'.join([src.stem, sdk, compiler, *METAL_FLAGS]).encode()).hexdigest() + '.metallib')
        how = 'kept'
        if not kept.is_file():
            with tempfile.TemporaryDirectory(dir=kept_dir) as d:
                air, lib = Path(d) / 'v.air', Path(d) / 'v.metallib'
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', *METAL_FLAGS, '-c', str(src), '-o', str(air)], env=env, capture_output=True, text=True)
                if not r.returncode: r = subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)], env=env, capture_output=True, text=True)
                if r.returncode or not lib.is_file(): return 'failed'
                os.replace(lib, kept)
            how = 'compiled'
        place(kept, seed / f'variant-{src.stem}.metallib')
        return how
    counts = {'compiled': 0, 'kept': 0, 'failed': 0}
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for how in pool.map(compile_one, sources): counts[how] += 1
    return {'variants': len(sources), **counts}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mac', type=Path, help='the pipeline-archive mode\'s pipelines folder (needed with --arch)')
    ap.add_argument('--seed', type=Path, required=True, help='the device seed (seed-manifest.json, the metallibs)')
    ap.add_argument('--cache', type=Path, required=True, help='kept scripts and compiled files, outside the app')
    ap.add_argument('--sdk', default='iphoneos')
    ap.add_argument('--arch', action='append', default=[], help='GPU architecture, e.g. applegpu_g14p (iPhone 14); none: remove them')
    ap.add_argument('--variants', type=Path, help='only compile the recorded variant sources of this folder into the seed (none: remove them)')
    a = ap.parse_args()
    env = os.environ.copy()
    seed = a.seed.resolve()
    if a.variants:
        print(json.dumps(variants(a.variants, seed, a.cache, a.sdk, env))); return
    for old in seed.glob('pipelines-*'):   # this build's set replaces the last one's
        old.unlink()
    if not a.arch:
        print(json.dumps({'archs': []})); return
    if not a.mac: ap.error('--arch needs --mac')
    manifest = json.loads((seed / 'seed-manifest.json').read_text())
    libs = {digest.upper(): name for name, digest in manifest['files'].items() if name.endswith('.metallib')}
    for p in seed.glob('variant-*.metallib'): libs[hashlib.sha256(p.read_bytes()).hexdigest().upper()] = p.name
    compiler = hashlib.sha256(Path(tool(a.sdk, 'metal-tt', env)).read_bytes()).hexdigest()
    scripts = a.cache / 'scripts'; scripts.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in a.mac.glob('*.metalarchive') if (p.with_suffix('.keys')).is_file())
    workers = os.cpu_count() or 4
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        extracted = list(pool.map(lambda p: script_of(p, a.sdk, scripts, env), files))
    report = {'mac_files': len(files), 'archs': []}
    jobs = []   # (name, keys, script with the seed's paths, digest)
    skipped = {'no script': 0, 'library from source': 0}
    for path, script in zip(files, extracted):
        if not script: skipped['no script'] += 1; continue
        paths = script.get('libraries', {}).get('paths', [])
        if not paths or any(p.get('label', '').upper() not in libs for p in paths):
            skipped['library from source'] += 1; continue
        for p in paths: p['path'] = ''
        digest = hashlib.sha256(json.dumps(script, sort_keys=True).encode()).hexdigest()
        for p in paths: p['path'] = str(seed / libs[p['label'].upper()])
        keys = [k for k in path.with_suffix('.keys').read_text().split('\n') if k]
        jobs.append((path.stem, keys, script, digest))
    for arch in a.arch:
        out_dir = a.cache / arch; out_dir.mkdir(parents=True, exist_ok=True)
        def compile_one(job):
            name, keys, script, digest = job
            kept = out_dir / f'{hashlib.sha256((digest + compiler).encode()).hexdigest()}.metalarchive'
            if kept.is_file(): return name, keys, kept, 'kept', ''
            with tempfile.TemporaryDirectory(dir=out_dir) as d:
                src = Path(d) / 'pipeline.mtlp-json'; src.write_text(json.dumps(script))
                tmp = Path(d) / 'out.metalarchive'
                r = subprocess.run(['xcrun', '-sdk', a.sdk, 'metal-tt', '-arch', arch, str(src), '-o', str(tmp)], env=env, capture_output=True, text=True)
                if r.returncode or not tmp.is_file():
                    return name, keys, None, 'failed', ((r.stderr or r.stdout).strip().splitlines() or ['?'])[-1][:200]
                os.replace(tmp, kept)
            return name, keys, kept, 'compiled', ''
        counts = {'compiled': 0, 'kept': 0, 'failed': 0}; reasons = {}
        index = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for i, (name, keys, kept, how, why) in enumerate(pool.map(compile_one, jobs), 1):
                counts[how] += 1
                if why: reasons[why] = reasons.get(why, 0) + 1
                if kept:
                    place(kept, seed / f'pipelines-{arch}-{name}.metalarchive')
                    index += [f'{k}\t{name}' for k in keys]
                if i % 1000 == 0: print(f'{arch}: {i}/{len(jobs)}', flush=True)
        (seed / f'pipelines-{arch}.tsv').write_text('\n'.join(index) + '\n')
        (seed / f'pipelines-{arch}.json').write_text(json.dumps({'schema': SCHEMA, 'chain': manifest['chain'], 'arch': arch,
                                                                  'pipelines': len(index)}, indent=1) + '\n')
        report['archs'].append({'arch': arch, 'pipelines': len(index), **counts, 'reasons': reasons})
    report.update(skipped)
    print(json.dumps(report))


if __name__ == '__main__':
    try: main()
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
