#!/usr/bin/env python3
"""Translate the shader variants an iPhone/iPad was missing and send them back to it.

iOS has no shader translator. On a miss the game skips the draws that need the variant and saves the complete request
(variant, stage, guest code) to Documents/shader-requests/ (third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m). This tool
pulls those requests, translates them with the Mac's runtime service (build/shader-service/totk_xlate) into the Mac
cache, compiles device metallibs, and pushes one pack in the startup-seed format to Documents/shader-fill/. The game
checks and merges the pack on its next start (totk_fill_import) and warms the new variants in the background.
usage: fill_shaders.py --tree <work tree> --device <id> [--bundle-id <id>] [--requests <dir>] [--with-done] [--no-push]"""
import argparse, concurrent.futures, hashlib, json, os, platform, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
from xcode_dir import developer_dir
from compile_seed import METAL_FLAGS

TOTK = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOTK/'scripts'))
from shader_list import ShaderList   # noqa: E402
SHADER_LIST = TOTK/'assets/shader_list.tsv'
CONTRIBUTION = Path(os.environ.get('MNG_CONTRIB_FOLDER') or TOTK.parent.parent/'shader-list-contribution')
CONTRIBUTION_HEADER = (
    '# Shaders and render pipelines this Mac and its iPhones / iPads met that the shared shader list does not have yet\n'
    '# (docs/shader-list.md). Only shader keys, bind states and pipeline descriptors: no game data or personal information.\n'
    '# Send it in a GitHub issue: https://github.com/ly05010419/MacNativeGames/issues/new?template=shader-list.yml\n')
VARIANT = re.compile(r'^([0-9a-f]{64})-[0-9a-f]{12}$')
HEX = re.compile(r'^(?:[0-9a-f]{2})+$')
MAX_CODE = 16 << 20   # the runtime's own limit on a shader's code area


def translator_service(tree):
    """The native translator: the work tree's build, else the checked-in binary for this Mac."""
    # platform.machine() follows the interpreter: a Python running under Rosetta reports x86_64 on an arm64 Mac.
    # The shipped binary is arm64 and the kernel runs it natively, so always consider arm64 as well.
    arches = [platform.machine()] + (['arm64'] if platform.machine() != 'arm64' else [])
    candidates = [tree/'build/shader-service/totk_xlate'] + \
                 [root/f'prebuilt/macos-{arch}/totk_xlate' for arch in arches for root in (tree, TOTK)]
    for path in candidates:
        if os.access(path, os.X_OK): return path
    raise RuntimeError('missing shader translator; looked for ' + ', '.join(str(p) for p in candidates))



def contribution_file():
    CONTRIBUTION.mkdir(parents=True, exist_ok=True)
    return CONTRIBUTION/'shader-list-contribution.txt'


def append_contribution(contrib, rows):
    """Append the rows the contribution file does not hold yet; returns how many were written.

    Rerunning for the same requests (--with-done rebuilds them on purpose) must not grow the file again: it is what
    players attach to an issue, and the row text is what shader_contrib_watch.merge_rows compares.
    """
    exists = contrib.exists()
    have = set(contrib.read_text(errors='replace').splitlines()) if exists else set()
    fresh = [row for row in dict.fromkeys(rows) if row not in have]
    if fresh:
        with contrib.open('a') as f:
            f.write(('' if exists else CONTRIBUTION_HEADER) + ''.join(f'{row}\n' for row in fresh))
    return len(fresh)


def chain():
    for line in (TOTK/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text().splitlines():
        if line.startswith('#define DMSH_CHAIN'): return line.split('"')[1]
    raise RuntimeError('DMSH_CHAIN not found')


def xcode_env():
    return dict(os.environ, DEVELOPER_DIR=developer_dir())


def devicectl(env, args, direction, src, dst):
    cmd = ['xcrun', 'devicectl', 'device', 'copy', direction, '--device', args.device, '--domain-type', 'appDataContainer',
           '--domain-identifier', args.bundle_id, '--source', str(src), '--destination', str(dst)]
    return subprocess.run(cmd, capture_output=True, text=True, env=env)


def missing_on_device(result):
    """The source path does not exist yet (no miss has been recorded): not a failed copy."""
    return 'Failed to retrieve the file node' in (result.stderr or '')


def load_requests(folder):
    """Requests come from the device: only well-formed ones are used."""
    good = []
    for path in sorted(Path(folder).glob('*.json')):
        try: rq = json.loads(path.read_text())
        except (OSError, ValueError) as e: print(f'skip {path.name}: {e}'); continue
        code = rq.get('code_hex') if isinstance(rq, dict) else None
        if (not isinstance(rq, dict) or not VARIANT.match(str(rq.get('variant'))) or rq.get('variant') != path.stem
                or rq.get('stage') not in ('vs', 'ps', 'cs') or not isinstance(code, str) or not HEX.match(code)
                or len(code) // 2 > MAX_CODE):
            print(f'skip {path.name}: not a shader request'); continue
        good.append(rq)
    return good


def translate(rqs, tree, out, ch):
    """Translate the variants the Mac cache does not have yet, as the runtime does (totk_rt_variant)."""
    todo = [rq for rq in rqs if not (out/f"{rq['variant']}_b.metal").is_file()]
    print(f'{len(rqs)} requests, {len(todo)} not yet translated on this Mac')
    if not todo: return set()
    service = translator_service(tree)
    spirv_cross = tree/'build/spirv-cross/spirv-cross'
    if not os.access(spirv_cross, os.X_OK): raise RuntimeError(f'missing {spirv_cross}')
    print(f'translator: {service}')
    svc = subprocess.Popen([str(service), '--service', '--spirv-cross', str(spirv_cross), '--out', str(out),
                            '--chain', ch], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    failed = set()
    try:
        for rq in todo:
            variant = rq['variant']; job = dict(rq)
            code = out/f'{variant}.code'; code.write_bytes(bytes.fromhex(job.pop('code_hex'))); job['code'] = str(code)
            svc.stdin.write(json.dumps(job, sort_keys=True) + '\n'); svc.stdin.flush()
            line = svc.stdout.readline()
            if not line: raise RuntimeError(f'shader service exited while translating {variant}')
            reply = json.loads(line)
            if not reply.get('ok'): failed.add(variant); print(f"FAILED {variant}: {reply.get('error')}")
    finally:
        svc.stdin.close(); svc.wait()
    return failed


def record(out, variant):
    """Add the variant to the Mac cache's index exactly as the runtime does; returns its index row."""
    row = {'variant': variant, 'deps': json.loads((out/f'{variant}_b.json').read_text()).get('deps') or {}}
    index = out/f"{variant.split('-')[0]}.variants"
    rows = json.loads(index.read_text()) if index.is_file() else []
    if not any(r.get('variant') == variant for r in rows):
        index.write_text(json.dumps(rows + [row], sort_keys=True, separators=(',', ':')))
    return row


def compile_metallib(pack, name, env):
    src = pack/f'{name}.metal'; air = pack/f'{name}.air'
    try:
        subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metal', *METAL_FLAGS, '-c', str(src), '-o', str(air)], check=True, capture_output=True, env=env)
        subprocess.run(['xcrun', '-sdk', 'iphoneos', 'metallib', str(air), '-o', str(pack/f'{name}.metallib')], check=True, capture_output=True, env=env)
        return None
    except subprocess.CalledProcessError as e:   # the device then compiles the MSL source itself
        return f"{name}: {e.stderr.decode(errors='replace').strip().splitlines()[-1:]}"
    finally:
        air.unlink(missing_ok=True)


def build_pack(pack, out, variants, ch, env):
    """The pack uses the startup-seed format so the device checks every file before merging it."""
    indexes = {}
    for variant in variants:
        for suffix in ('_b.metal', '_b.json'): shutil.copy2(out/f'{variant}{suffix}', pack/f'{variant}{suffix}')
        indexes.setdefault(variant.split('-')[0], []).append(record(out, variant))
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as pool:
        for problem in pool.map(lambda v: compile_metallib(pack, f'{v}_b', env), variants):
            if problem: print(f'no iOS metallib, the device compiles the source: {problem}')
    for shader, rows in indexes.items():
        (pack/f'{shader}.variants').write_text(json.dumps(rows, sort_keys=True, separators=(',', ':')))
    files = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(pack.iterdir()) if f.is_file()}
    (pack/'seed-manifest.json').write_text(json.dumps({'schema': 1, 'chain': ch, 'shaders': len(variants), 'files': files}, indent=2) + '\n')
    return len(files)


def main():
    ap = argparse.ArgumentParser(description='Translate the shaders an iOS device missed and push them back to it.')
    ap.add_argument('--tree', type=Path, required=True, help='prepared work tree (build/shader-service, build/spirv-cross)')
    ap.add_argument('--device', default=os.environ.get('TOTK_IOS_DEVICE'), help='devicectl device id (or TOTK_IOS_DEVICE)')
    ap.add_argument('--bundle-id', default=os.environ.get('TOTK_BUNDLE_ID', 'org.macnativegames.totk'))
    ap.add_argument('--requests', type=Path, help='use requests already copied from the device instead of pulling them')
    ap.add_argument('--no-push', action='store_true', help='translate and build the pack, but do not send it')
    ap.add_argument('--update-list', action='store_true',
                    help='also write the new variants into games/totk/assets/shader_list.tsv (the shipped list); '
                         'by default they only go to the contribution file')
    ap.add_argument('--with-done', action='store_true', help='also rebuild the variants filled before (shader-requests/done),'
                    ' e.g. after a change of the compile options; the game replaces their libraries')
    args = ap.parse_args()
    if not args.requests and not args.device: raise RuntimeError('give --device (or TOTK_IOS_DEVICE), or --requests')
    if not args.no_push and not args.device: raise RuntimeError('pushing needs --device (or TOTK_IOS_DEVICE); use --no-push')
    tree = args.tree.resolve(); ch = chain(); env = xcode_env()
    out = tree/f'build/shader-cache/rt-{ch}'; out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='totk-shader-fill-'))
    try:
        folder = args.requests
        incomplete = False
        if not folder:
            folder = work/'requests'
            r = devicectl(env, args, 'from', 'Documents/shader-requests', folder)
            if r.returncode:
                if missing_on_device(r):
                    print('no shader requests from the device'); return
                incomplete = True
                print(f"pull was incomplete, using what arrived: {r.stderr.strip().splitlines()[-1:]}")
        rqs = load_requests(folder)
        if args.with_done:
            done = folder/'done'
            if not args.requests and not done.is_dir():
                r = devicectl(env, args, 'from', 'Documents/shader-requests/done', done)
                if r.returncode and not missing_on_device(r):
                    incomplete = True   # a disconnect here must not read as "nothing to do"
                    print(f"pull of done requests was incomplete: {r.stderr.strip().splitlines()[-1:]}")
            seen = {rq['variant'] for rq in rqs}
            rqs += [rq for rq in load_requests(done) if rq['variant'] not in seen]
        if not rqs:
            # An incomplete pull that recovered nothing is a connection problem, not "the device has no requests":
            # the wrapper would otherwise report success and the user would never retry.
            if incomplete: raise RuntimeError('could not read Documents/shader-requests; unlock the device, keep it connected and retry')
            print('no shader requests from the device'); return
        failed = translate(rqs, tree, out, ch)
        variants = sorted({rq['variant'] for rq in rqs} - failed)
        if not variants: raise RuntimeError('no variant could be translated')
        name = time.strftime('%Y%m%d-%H%M%S'); pack = work/name; pack.mkdir()
        count = build_pack(pack, out, variants, ch, env)
        # What this device needed leaves the repository alone by default: the rows go to the contribution file the
        # project already submits through a GitHub issue (shader_contrib_watch.py). --update-list also writes the
        # shipped list, which every build reads, for someone who wants the next local build to carry the variant.
        # The pack's index rows are written by build_pack, so the list is merged from the pack after it is built.
        shared = ShaderList.load(SHADER_LIST); before = set(shared.variants); shared.add_cache(pack)
        new = sorted(shared.variants - before)
        if new:
            added = append_contribution(contribution_file(), [f'V\t{key}\t{deps}' for key, deps in new])
            if added:
                print(f'contribution file: {added} new variants ({CONTRIBUTION}; shader_contrib_watch.py sends it in an issue)')
        if args.update_list:
            shared.save(SHADER_LIST)
            print(f'shared shader list: {len(new)} new variants ({SHADER_LIST})')
        print(f'pack {name}: {len(variants)} variants, {count} files' + (f', {len(failed)} failed' if failed else ''))
        if args.no_push:
            keep = tree/'build/shader-fill'/name; keep.parent.mkdir(parents=True, exist_ok=True); shutil.move(str(pack), keep)
            print(f'not pushed; the pack is in {keep}'); return
        r = devicectl(env, args, 'to', pack, f'Documents/shader-fill/{name}')
        if r.returncode: raise RuntimeError(f"push failed: {r.stderr.strip()[-400:]}")
        print('pushed; restart the game on the device to merge and warm the new shaders')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    try: main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e: print(f'ERROR: {e}', file=sys.stderr); sys.exit(1)
