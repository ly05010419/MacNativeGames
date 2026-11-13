#!/usr/bin/env python3
"""The device's pipeline archive, compiled on the Mac while packaging (2026-10-29).

build_ipa.py first runs the Mac game binary's pipeline-archive mode, which makes every pipeline of the shader lists from
the device seed's metallibs, one Metal archive file each, as the device's own first start does (nvn_mtl.m
mtl_pipeline_archive_tool). This script compiles those pipelines into the code of one device GPU with metal-tt and puts
the results into the seed, flat (a pushed seed is one stream of files: install_device.py write_pack):

  pipelines-<arch>-<name>.metalarchive  the GPU code of the pipelines of one group
  pipelines-<arch>.tsv                  "<key>\t<name>" for every key of those files
  pipelines-<arch>.json                 schema, chain, arch: the runtime takes them only when all three match
                                        (totk_pipeline_archive.h par_load_prebuilt)

Merged files (2026-11-13, --groups N; GROUPS by default): one file per Mac file put 19,750 files into the iPad (A16)
build, and the game was killed during warm-up by jetsam reason "vnode-limit" (about 11,400 vnodes; each archive the
runtime opens holds one). --groups N merges the Mac files' pipeline scripts into N groups by the hash of the Mac file's
name (group_of), one metal-tt run each: the pipeline lists put together and each library once (merge). metal-tt writes
all of them into one archive (its AIR_PIPELINE note counts them: 599 of 599 on the A16), and the runtime contract is
unchanged: several keys simply name the same file. The hash, not runs of sorted names, so that a pipeline added or
dropped changes only its own group's file. A group metal-tt refuses falls back to one file per Mac file and is reported
(group_fallbacks); a pipeline that fails on its own too is left out (the device builds it on its first start).
On by default (GROUPS = 10, about 2,000 pipelines a file, nxnative's ARCH_PERIOD_MAX): with a file per pipeline
the game still died of vnode-limit with at most 256 archives open (iPad A16, JetsamEvent-2026-11-13-104502: 7,098
mapped regions, as a pipeline made from an archive keeps that file's code mapped, so closing the archive object frees
no vnode). The memory the first measurement saw (an opened archive costing its whole file: 599 one-pipeline files
+113.8 MB for 107 MB, merged 32 / 75 / 150 pipelines +5.7 / 14.6 / 27.3 MB) was the drive, not the archive: Metal reads
an archive through NSData with NSDataReadingMappedIfSafe, and Foundation reads a file on an external USB drive into
memory whole but maps one on the internal disk (nxnative docs/reference/memory.md, "A pipeline archive on an external
drive is read into memory"; the measurement ran on /Volumes/开发, an external drive). The iPad's files are on its
internal storage: mapped, clean pages.
Time is the same either way (M4, 599 A16 pipelines: one file each 73 s on 10 threads; 2 groups of about 300, 2 at a
time, 73 s), since metal-tt's backend (applegpu-nt) runs one script on about 4 cores with 2-3 GB: GROUP_JOBS at once.

--variants <folder>: first, the patched variants (border colours, tile reads, ...) the runtime compiles from source at
run time, recorded by a first run of that mode (nvn_mtl.m variant_record: <SHA-256 of the source>.metal), are compiled
for the device into the seed as variant-<SHA-256>.metallib, with the seed's own options (compile_seed.py METAL_FLAGS).
The device loads them instead of compiling them (nvn_mtl.m variant_library_seed), and a second run of the mode makes
their pipelines from them, so these can be compiled for the GPU too (BotW 1.6.0: 1,111 pipelines, 58 s on an iPhone
14's first start before). A pipeline whose functions still come from a library the seed does not have is left out;
the device builds it on its first start. Results are kept by the content of their pipeline script (a group's: its
members' scripts) or source and the compiler (<cache>/<arch>/, <cache>/variant-libs/), so a build whose lists and shaders did not change compiles nothing again.
Measured: BotW 1.6.0, 13,760 pipelines; metal-tt takes about 0.4 s per pipeline on an M4, run on every core.
"""
import argparse, concurrent.futures, ctypes, errno, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path
from compile_seed import METAL_FLAGS, metal_flags, target_flags

SCHEMA = 6   # totk_pipeline_archive.h PAR_SCHEMA
GROUPS = 10   # merged files per GPU (about 2,000 pipelines each); 0: one file per Mac file, as before 2026-11-13
MERGE = 1    # merge()'s layout: part of a group's cache key
GROUP_JOBS = max(2, (os.cpu_count() or 4) // 4)

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


def seed_check(seed, arch):
    """Whether a seed carries complete pipelines for the GPU architecture arch (2026-11-01: an iPhone was installed with
    an iPad's, and filled its own archive for minutes on its first start). ('ok' | 'none' | 'wrong', a line to print):
    none = pipelines for no GPU (built without a device on the cable; the first start fills them, as before 2026-10-29),
    wrong = pipelines for other GPUs only, or this GPU's index, stamp and files do not agree."""
    seed = Path(seed)
    carried = sorted(p.name[len('pipelines-'):-len('.json')] for p in seed.glob('pipelines-*.json'))
    if arch not in carried:
        if not carried:
            return 'none', (f'pipelines: the app carries none (built without an iPhone / iPad on the cable), so the first start on '
                            f'this {arch} device builds them (about 10 minutes); build with it on the cable to have them made on the Mac')
        return 'wrong', (f'pipelines: the app carries them for {", ".join(carried)}, not for this device\'s {arch}: its first start '
                         f'would build them all (about 10 minutes); build again with this device on the cable')
    try:
        stamp = json.loads((seed/f'pipelines-{arch}.json').read_text())
        rows = [l.split('\t') for l in (seed/f'pipelines-{arch}.tsv').read_text().splitlines() if l]
    except (OSError, ValueError) as e:
        return 'wrong', f'pipelines for {arch}: unreadable ({e})'
    if stamp.get('arch') != arch or stamp.get('schema') != SCHEMA or stamp.get('pipelines') != len(rows):
        return 'wrong', f'pipelines for {arch}: the stamp {stamp} does not match (schema {SCHEMA}, {len(rows)} rows)'
    names = {r[1] for r in rows if len(r) == 2}
    missing = sorted(n for n in names if not (seed/f'pipelines-{arch}-{n}.metalarchive').is_file())
    if len(names) != len({r[1] for r in rows}) or missing:
        return 'wrong', f'pipelines for {arch}: {len(missing)} of {len(names)} files missing (e.g. {missing[:1]})'
    return 'ok', f'pipelines: the app carries {len(rows)} for this device\'s GPU ({arch}) in {len(names)} files, made on the Mac'


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


_clonefile = getattr(ctypes.CDLL(None, use_errno=True), 'clonefile', None)


def place(src, dst):
    """An APFS clone of src at dst (clonefile(2), what cp -c does), else a copy. In the process: a cp per file took 37 of
    the 55 s a build spent when every pipeline of an iPhone was kept (2026-11-01, 15,038 files)."""
    if _clonefile:
        s, d = os.fsencode(src), os.fsencode(dst)
        if _clonefile(s, d, 0) == 0: return
        if ctypes.get_errno() == errno.EEXIST:
            os.unlink(dst)
            if _clonefile(s, d, 0) == 0: return
    shutil.copyfile(src, dst)


def mac_cache(folder, want):
    """How build_ipa.py's run of the Mac binary's pipeline-archive mode treats its cache folder; returns the environment
    it adds ({'TOTK_PAR_RECHECK': '1' or '0'}). The folder is kept from one build to the next, also when the shaders,
    lists or runtime changed (as build_app.mac_pipelines; user 2026-11-13: "根治，写测试 以后保证这样"): with
    TOTK_PAR_RECHECK=1 the run makes every listed pipeline against the files there, compiles and writes only the ones
    described otherwise now (nvn_mtl.m par_stale) and removes the files it fully superseded (par_prune_superseded). The
    files it keeps keep their bytes, so their metal-tt groups are kept here too. Until then any change, the runtime's
    included, threw the folder away: Ocarina of Time for an iPad (2026-11-13) made its 19,679 pipelines from scratch twice,
    240 s + 228 s, and compiled every group again. A recheck makes every pipeline and so records every variant again
    (nvn_mtl.m variant_record), so the recorded ones (variants/) go first: none no pipeline uses any more is packaged.
    `want` is what the folder is made from (shaders, lists, runtime); the run's caller writes it to seed.txt when done."""
    stamp = folder / 'seed.txt'
    recheck = not (stamp.is_file() and stamp.read_text().strip() == want)
    if recheck: shutil.rmtree(folder / 'variants', ignore_errors=True)
    folder.mkdir(parents=True, exist_ok=True)
    return {'TOTK_PAR_RECHECK': '1' if recheck else '0'}


def variants(folder, seed, cache, sdk, env):
    """The recorded variant sources compiled into the seed as variant-<SHA-256>.metallib (see the top); the last build's
    that are not recorded now are removed. Returns the report."""
    for old in seed.glob('variant-*'): old.unlink()
    sources = [p for p in sorted(folder.glob('*.metal')) if len(p.stem) == 64 and hashlib.sha256(p.read_bytes()).hexdigest() == p.stem] if folder.is_dir() else []
    compiler = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], env=env, check=True, capture_output=True, text=True).stdout
    kept_dir = cache / 'variant-libs'; kept_dir.mkdir(parents=True, exist_ok=True)
    def compile_one(src):
        kept = kept_dir / (hashlib.sha256('\0'.join([src.stem, sdk, compiler, *METAL_FLAGS, *target_flags(sdk)]).encode()).hexdigest() + '.metallib')
        how = 'kept'
        if not kept.is_file():
            with tempfile.TemporaryDirectory(dir=kept_dir) as d:
                air, lib = Path(d) / 'v.air', Path(d) / 'v.metallib'
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', *metal_flags(src), *target_flags(sdk), '-c', str(src), '-o', str(air)], env=env, capture_output=True, text=True)
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


def mergeable(script, like):
    """Whether a pipeline script can share a merged script with like (the group's first): only what merge() knows how to
    combine (libraries by label, pipeline lists, no function list), of the same format."""
    return (set(script) <= {'version', 'generator', 'libraries', 'pipelines', 'functions'} and not script.get('functions')
            and script.get('version') == like.get('version') and script.get('generator') == like.get('generator'))


def merge(scripts):
    """One pipeline script with every pipeline of scripts, in their order, and each library they name once (by label):
    metal-tt then writes them all into one archive (checked 2026-11-13: the archive's AIR_PIPELINE note counts them)."""
    paths, seen, pipelines = [], set(), {}
    for s in scripts:
        for p in s.get('libraries', {}).get('paths', []):
            if p['label'].upper() not in seen: seen.add(p['label'].upper()); paths.append(dict(p))
        for kind, entries in s.get('pipelines', {}).items(): pipelines.setdefault(kind, []).extend(entries)
    return {'version': scripts[0].get('version'), 'generator': scripts[0].get('generator'), 'libraries': {'paths': paths},
            'pipelines': pipelines, 'functions': {}}


def group_of(name, groups):
    """The merged file a Mac file's pipelines go into: by the hash of its name, so a pipeline added or dropped changes
    only its own group's file and every other group is kept from the cache (consecutive runs of sorted names would move
    every later boundary)."""
    return int(hashlib.sha256(name.encode()).hexdigest()[:8], 16) % groups


def plan_groups(jobs, groups):
    """The jobs (name, keys, script, digest) as [(file name, digest, members)], sorted by file name, each group's members
    by name. A group's file is named by its number and the digest of its content (the merge layout and every member's
    script), which is also its cache key. A script merge() cannot combine with its group's first keeps a file of its
    own, named and kept as before 2026-11-13."""
    buckets, own = {}, []
    for job in sorted(jobs, key=lambda j: j[0]):
        b = group_of(job[0], groups)
        if mergeable(job[2], buckets[b][0][2] if b in buckets else job[2]): buckets.setdefault(b, []).append(job)
        else: own.append((job[0], job[3], [job]))
    planned = own
    for b, members in buckets.items():
        digest = hashlib.sha256(f'merge:{MERGE}\0'.encode() + '\0'.join(m[3] for m in members).encode()).hexdigest()
        planned.append((f'g{b}-{digest[:16]}', digest, members))
    return sorted(planned, key=lambda g: g[0])


def metal_tt(script, kept, sdk, arch, env):
    """Compiles a pipeline script for arch into kept: '' when done, else the last line metal-tt printed."""
    with tempfile.TemporaryDirectory(dir=kept.parent) as d:
        src = Path(d) / 'pipeline.mtlp-json'; src.write_text(json.dumps(script))
        tmp = Path(d) / 'out.metalarchive'
        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal-tt', '-arch', arch, str(src), '-o', str(tmp)], env=env, capture_output=True, text=True)
        if r.returncode or not tmp.is_file():
            return ((r.stderr or r.stdout).strip().splitlines() or ['?'])[-1][:200]
        os.replace(tmp, kept)
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mac', type=Path, help='the pipeline-archive mode\'s pipelines folder (needed with --arch)')
    ap.add_argument('--seed', type=Path, required=True, help='the device seed (seed-manifest.json, the metallibs)')
    ap.add_argument('--cache', type=Path, required=True, help='kept scripts and compiled files, outside the app')
    ap.add_argument('--sdk', default='iphoneos')
    ap.add_argument('--arch', action='append', default=[], help='GPU architecture, e.g. applegpu_g14p (iPhone 14); none: remove them')
    ap.add_argument('--groups', type=int, default=GROUPS, help=f'merged files per GPU (default {GROUPS}; 0: one file per Mac file, as before 2026-11-13)')
    ap.add_argument('--group-jobs', type=int, default=GROUP_JOBS, help=f'merged files compiled at once (default {GROUP_JOBS}: metal-tt runs one on several cores)')
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
    planned = plan_groups(jobs, a.groups) if a.groups > 0 else [(j[0], j[3], [j]) for j in jobs]
    for arch in a.arch:
        out_dir = a.cache / arch; out_dir.mkdir(parents=True, exist_ok=True)
        def compile_one(name, digest, members):
            """One file of members' pipelines: (name, keys, kept or None, how, why)."""
            keys = [k for m in members for k in m[1]]
            kept = out_dir / f'{hashlib.sha256((digest + compiler).encode()).hexdigest()}.metalarchive'
            if kept.is_file(): return name, keys, kept, 'kept', ''
            why = metal_tt(merge([m[2] for m in members]) if len(members) > 1 else members[0][2], kept, a.sdk, arch, env)
            return name, keys, None if why else kept, 'failed' if why else 'compiled', why
        counts = {'compiled': 0, 'kept': 0, 'failed': 0}; reasons = {}
        index, placed, fallback = [], 0, []
        def take(result, members):
            nonlocal placed
            name, keys, kept, how, why = result
            counts[how] += len(members)
            if why: reasons[why] = reasons.get(why, 0) + len(members)
            if kept:
                place(kept, seed / f'pipelines-{arch}-{name}.metalarchive'); placed += 1
                index.extend((k, name) for k in keys)
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers if a.groups <= 0 else max(1, a.group_jobs)) as pool:
            for (name, digest, members), result in zip(planned, pool.map(lambda g: compile_one(*g), planned)):
                done += len(members)
                if result[2] or len(members) == 1: take(result, members)
                else:   # the group failed as one: each Mac file on its own, as before 2026-11-13
                    print(f'{arch}: group {name} ({len(members)} pipelines) failed as one file ({result[4]}); compiling them one by one', flush=True)
                    fallback.append(name)
                    reasons[f'group: {result[4]}'] = reasons.get(f'group: {result[4]}', 0) + 1
                    for m, r in zip(members, pool.map(lambda m: compile_one(m[0], m[3], [m]), members)): take(r, [m])
                if done // 1000 != (done - len(members)) // 1000 or done == len(jobs):
                    print(f'{arch}: {done}/{len(jobs)} pipelines, {placed} files', flush=True)
        (seed / f'pipelines-{arch}.tsv').write_text(''.join(f'{k}\t{n}\n' for k, n in sorted(index)))
        (seed / f'pipelines-{arch}.json').write_text(json.dumps({'schema': SCHEMA, 'chain': manifest['chain'], 'arch': arch,
                                                                  'pipelines': len(index)}, indent=1) + '\n')
        report['archs'].append({'arch': arch, 'pipelines': len(index), 'files': placed, 'groups': len(planned),
                                'group_fallbacks': len(fallback), **counts, 'reasons': reasons})
    report.update(skipped)
    print(json.dumps(report))


if __name__ == '__main__':
    try: main()
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
