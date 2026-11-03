#!/usr/bin/env python3
# The pipeline archive made offline: the base of every shader pair, as the host's pipeline prebuild makes it on the device (pre_desc / pre_pair in dm/gfx/nvn/nvn_mtl.m), compiled on the Mac
# by metal-tt into one Metal 4 pipeline archive per GPU architecture, so an iPhone takes the bases from the app bundle instead of compiling them on its first start.
# The host matches libraries by content, so the archive serves the libraries the app ships (msl/b-<chain>); measurements are in dm/games/01007EF00011E000/README.md.
# Per pair (vertex key, pixel key) of the profile's shader_pairs.tsv and pso_pairs.tsv, read in that order with repeated lines dropped, as the host reads them:
#   · the vertex shader's pulling library <key>_bp (a vertex shader without stage inputs: its own <key>_b), entry `entry`, or `entry_z` for the near = 0 depth mode, from its reflection
#     <key>_b.json; the pixel shader's <key>_b, entry `entry` (main0); no vertex descriptor, one sample, every colour attachment unspecialized
#   · left out as the host leaves them out (to the draws): a reflection of another chain version or none, a vertex shader with stage inputs and no pulling library, a vertex library that
#     writes the layer (render_target_array_index), a missing library or entry point
#   · a pair whose functions declare function constants is left to the device (rest_pairs.tsv): the host specializes such functions with an empty set of values, and metal-tt makes
#     no specialized function from a library (applegpu-nt: "cannot find private metadata"), nor the function unspecialized ("cannot lower module with unresolved function constants")
# Output folder (what the app bundles as pso/, the host's DM_PSO_BUNDLE):
#   <arch>.mtl4a     one archive per GPU architecture (applegpu_g18p = A19 / A19 Pro); the host opens the one named after its device's architecture
#   prebuilt.tsv     what the archives cover, in the host's stamp format (pso_prebuild): chain version, depth mode, the pair lists' size and SHA-256. The host skips its prebuild when
#                    it matches and a probe of a few bases finds them in the archive
#   rest_pairs.tsv   the pairs the device still builds (only when there are any)
#   archive.tsv      what was built: architectures, OS minimum, toolchain, bases (for people and logs; the host does not read it)
# Each archive is made again only when its key changes (key.<arch> in the work folder): this file, the chain version, the libraries' bytes and stamp (inputs.tsv), the stamp above,
# the architecture, the platform and minimum OS, and metal-tt's version.
# Usage: pso_offline.py --msl <b-<chain> folder> --out <folder> --work <folder> [--game <id>] [--pairs <shader_pairs.tsv> [--pairs <pso_pairs.tsv>]] [--gpu g18p[,g17p]]
#          [--depth gl|zero] [--platform ios|macos] [--os-min 27.0] [--script-only]
#   --game: the profile (DM_GAMES_DIR as for game.py) whose shader_pairs.tsv and pso_pairs.tsv are the pairs, and whose depth_mode key the depth mode, unless --pairs / --depth say;
#   --gpu: architectures, with or without the applegpu_ prefix (default g18p); --depth: the game's NVN depth mode (gl = near -w, NVN's default; zero = near 0, which the game sets
#   with nvnDeviceSetDepthMode: the host knows it only at run time, so the profile says it; a wrong one costs the device its own prebuild, as without the archive);
#   --os-min: the archives' minimum OS (default: the SDK's major version, e.g. 27.0; a device on an older system cannot open them and builds the bases itself)
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, time
from pathlib import Path

LAYER = b'render_target_array_index'
COLOUR = ('pixel_format', 'write_mask', 'blending_state', 'alpha_blend_operation', 'rgb_blend_operation', 'destination_alpha_blend_factor', 'destination_rgb_blend_factor',
          'source_alpha_blend_factor', 'source_rgb_blend_factor')


def arch_name(a):   # 'g18p' → 'applegpu_g18p'
    a = a.strip()
    return a if a.startswith('applegpu_') else f'applegpu_{a}'


def pair_text(paths):
    """The pair lists as the host reads them: their bytes one after the other (shader_pairs.tsv, then pso_pairs.tsv when it exists)"""
    return b''.join(Path(p).read_bytes() for p in paths if p and Path(p).is_file())


def read_pairs(text):
    """(vertex key, pixel key) per line, in order, without empty lines, comments and repeated lines (pso_prebuild's reading)"""
    seen, out = set(), []
    for line in text.decode('utf-8').split('\n'):
        if not line or line.startswith('#') or line in seen: continue
        seen.add(line); c = line.split('\t')
        if len(c) >= 2: out.append((c[0], c[1]))
    return out


def stamp(chain, zero, text):
    """The host's prebuild stamp (pso_prebuild in nvn_mtl.m): chain version, depth mode, and the pair lists' length in bytes and SHA-256 (first 16 hex digits)"""
    return f'chain\t{chain}\ndepth\t{"near=0" if zero else "near=-w"}\npairs\t{len(text)}\t{hashlib.sha256(text).hexdigest()[:16]}\n'


def functions(path):
    """{entry name: declares function constants} of a .metallib, from its header: the function list (NAME tags) and each function's public metadata (a CNST tag lists its constants).
    Raises ValueError on a file it cannot read"""
    with open(path, 'rb') as f: b = f.read(1 << 20)
    if b[:4] != b'MTLB' or len(b) < 0x58: raise ValueError(f'{path}: not a Metal library')
    fo, _, po, ps = struct.unpack_from('<QQQQ', b, 0x18)
    def tags(p, end):
        t = {}
        while p + 4 <= end:
            tag = b[p:p + 4]; p += 4
            if tag == b'ENDT': return t
            n = struct.unpack_from('<H', b, p)[0]; p += 2; t[tag] = b[p:p + n]; p += n
        raise ValueError(f'{path}: unterminated tag list')
    try:
        count = struct.unpack_from('<I', b, fo)[0]; p = fo + 4; names = []
        for _ in range(count):   # each entry: its size (counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]; names.append(tags(p + 4, p + size)[b'NAME'].rstrip(b'\0').decode()); p += size
        consts, p = [], po
        for _ in range(count):   # each entry: its size (not counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]; consts.append(b'CNST' in tags(p + 4, p + 4 + size)); p += 4 + size
        if p != po + ps: raise ValueError(f'{path}: public metadata does not match the function list')
    except (struct.error, KeyError) as e: raise ValueError(f'{path}: unreadable function list ({e})') from e
    return dict(zip(names, consts))


def plan(lib_dir, chain, pairs, zero):
    """The bases the archive holds: [(vertex library, vertex entry, pixel library, pixel entry)], the pairs left to the device (function constants), and counts of the pairs left out"""
    lib_dir = Path(lib_dir); js, fns, layer = {}, {}, {}
    def refl(key):
        if key not in js:
            try: js[key] = json.loads((lib_dir / f'{key}_b.json').read_bytes())
            except (OSError, ValueError): js[key] = None
        return js[key]
    def fn(lib):
        if lib not in fns:
            try: fns[lib] = functions(lib_dir / f'{lib}.metallib')
            except (OSError, ValueError): fns[lib] = None
        return fns[lib]
    def writes_layer(lib):
        if lib not in layer: layer[lib] = LAYER in (lib_dir / f'{lib}.metallib').read_bytes()
        return layer[lib]
    bases, rest, seen, left = [], [], set(), dict.fromkeys(('reflection', 'stage inputs', 'library', 'layer', 'entry'), 0)
    for vk, fk in pairs:
        vj, fj = refl(vk), refl(fk)
        if not vj or not fj or vj.get('chain') != chain or fj.get('chain') != chain: left['reflection'] += 1; continue
        ventry = vj['entry_z'] if zero and vj.get('entry_z') else vj.get('entry'); fentry = fj.get('entry') or 'main0'
        pull = isinstance(vj.get('pull'), dict)
        if not pull and vj.get('vertex_inputs'): left['stage inputs'] += 1; continue
        vl, fl = vk + ('_bp' if pull else '_b'), fk + '_b'
        vf, ff = fn(vl), fn(fl)
        if vf is None or ff is None: left['library'] += 1; continue
        if writes_layer(vl): left['layer'] += 1; continue
        if ventry not in vf or fentry not in ff: left['entry'] += 1; continue
        if vf[ventry] or ff[fentry]: rest.append((vk, fk)); continue
        b = (vl, ventry, fl, fentry)
        if b not in seen: seen.add(b); bases.append(b)
    return bases, rest, left


def script(lib_dir, bases):
    """A Metal 4 pipelines script (metal4-pipelines-script(5), the .mtl4-json format metal-tt reads) with one render pipeline descriptor per base"""
    libs, fns, pipes = {}, {}, []
    def fd(lib, entry):
        if lib not in libs: libs[lib] = {'label': lib, 'path': str(Path(lib_dir).resolve() / f'{lib}.metallib')}
        label = f'{lib}.{entry}'
        if label not in fns: fns[label] = {'label': label, 'library': f'lib:{lib}', 'name': entry}
        return f'fnd:{label}'
    for vl, ve, fl, fe in bases:
        pipes.append({'vertex_function_descriptor': fd(vl, ve), 'fragment_function_descriptor': fd(fl, fe),
                      'color_attachments': [dict.fromkeys(COLOUR, 'Unspecialized') for _ in range(8)]})
    return {'libraries': list(libs.values()), 'function_descriptors': {'library_function_descriptors': list(fns.values())},
            'pipeline_descriptors': {'render_pipeline_descriptors': pipes}}


def library_digest(lib_dir, bases):
    """The bytes metal-tt consumes: a profile/chain stamp does not change when the same shaders are rebuilt by another Metal compiler."""
    h = hashlib.sha256()
    for name in sorted({lib for vl, _, fl, _ in bases for lib in (vl, fl)}):
        h.update(name.encode() + b'\0')
        lib = hashlib.sha256()
        with open(Path(lib_dir) / f'{name}.metallib', 'rb') as f:
            while chunk := f.read(1 << 20): lib.update(chunk)
        h.update(lib.digest())
    return h.hexdigest()


def sdk(platform):
    return 'iphoneos' if platform == 'ios' else 'macosx'


def tool_version(platform):
    r = subprocess.run(['xcrun', '-sdk', sdk(platform), 'metal-tt', '-version'], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'[RED] pso_offline: metal-tt is not available ({(r.stderr or r.stdout).strip()}); install the Metal toolchain: xcodebuild -downloadComponent MetalToolchain')
    return r.stdout.strip().splitlines()[0]


def os_min_default(platform):
    v = subprocess.run(['xcrun', '-sdk', sdk(platform), '--show-sdk-version'], capture_output=True, text=True, check=True).stdout.strip()
    return v.split('.')[0] + '.0'


def main(argv=None):
    ap = argparse.ArgumentParser(description='Build the pipeline archive of a game\'s shader pairs offline (metal-tt)')
    ap.add_argument('--msl', required=True, help='the shader libraries, b-<chain version> (with their reflection <key>_b.json)')
    ap.add_argument('--game', help='the game profile: its pair lists and depth_mode key')
    ap.add_argument('--pairs', action='append', help='shader_pairs.tsv, then pso_pairs.tsv (missing files are skipped; default: the profile\'s)')
    ap.add_argument('--out', required=True, help='the folder the app bundles (archives and stamp)')
    ap.add_argument('--work', required=True, help='the script, keys and logs')
    ap.add_argument('--gpu', default='g18p', help='GPU architectures, comma-separated (default g18p)')
    ap.add_argument('--depth', choices=('gl', 'zero'), help='the game\'s NVN depth mode (default: the profile\'s depth_mode, else gl: near = -w)')
    ap.add_argument('--platform', choices=('ios', 'macos'), default='ios')
    ap.add_argument('--os-min', help='minimum OS of the archives (default: the SDK\'s major version)')
    ap.add_argument('--script-only', action='store_true', help='write the script and the stamp, run no metal-tt')
    a = ap.parse_args(argv)
    if a.game:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games')); import game
        g = game.load(a.game)
        a.pairs = a.pairs or [str(g.table('shader_pairs.tsv')), str(g.table('pso_pairs.tsv'))]
        a.depth = a.depth or g.get('depth_mode', 'gl')
        if a.depth not in ('gl', 'zero'): sys.exit(f'[RED] pso_offline: {g.id}: depth_mode must be gl or zero: {a.depth}')
    if not a.pairs: sys.exit('[RED] pso_offline: no pair lists (--pairs or --game)')
    a.depth = a.depth or 'gl'
    lib_dir, out, work = Path(a.msl), Path(a.out), Path(a.work)
    chain = lib_dir.name[2:] if lib_dir.name.startswith('b-') else sys.exit(f'[RED] pso_offline: {lib_dir} is not a b-<chain version> folder')
    archs = sorted({arch_name(x) for x in a.gpu.split(',') if x.strip()})
    if not archs: sys.exit('[RED] pso_offline: no GPU architecture (--gpu)')
    text = pair_text(a.pairs)
    if not text: sys.exit(f'[RED] pso_offline: no pair list ({", ".join(a.pairs)})')
    zero = a.depth == 'zero'
    t0 = time.time()
    pairs = read_pairs(text)
    bases, rest, left = plan(lib_dir, chain, pairs, zero)
    work.mkdir(parents=True, exist_ok=True); out.mkdir(parents=True, exist_ok=True)
    js = work / 'bases.mtl4-json'   # metal-tt takes the format from the extension: .mtl4-json is Metal 4's (.mtlp-json is the Metal 3 format)
    body = json.dumps(script(lib_dir, bases), indent=1).encode()
    if not js.is_file() or js.read_bytes() != body: js.write_bytes(body)
    st = stamp(chain, zero, text)
    print(f'pso_offline: {len(pairs)} pairs: {len(bases)} bases, {len(rest)} left to the device (function constants), left out as the host leaves them: '
          + ', '.join(f'{k} {v}' for k, v in left.items()) + f' ({time.time() - t0:.1f} s)')
    if a.script_only: (out / 'prebuilt.tsv').write_text(st); return 0
    if pairs and not bases and not rest: sys.exit('[RED] pso_offline: no base to build')   # pairs, but the host would leave out every one: wrong or missing libraries
    if not pairs: print('pso_offline: no pairs (only a header): nothing to build, no archive')   # a profile whose split found no pair (GitHub issue #338)
    if not bases: archs = []   # no pair, or every eligible pair needs device specialization; clear old archives and still write the stamp and the remainder list
    osmin = a.os_min or os_min_default(a.platform); tv = tool_version(a.platform) if bases else 'not used (function constants only)'
    inputs = (lib_dir / 'inputs.tsv').read_text() if (lib_dir / 'inputs.tsv').is_file() else ''
    libraries = library_digest(lib_dir, bases)
    for arch in archs:
        key = hashlib.sha256('\0'.join([Path(__file__).read_text(), chain, inputs, libraries, st, arch, a.platform, osmin, tv, hashlib.sha256(body).hexdigest()]).encode()).hexdigest()[:16]
        dst, kf = out / f'{arch}.mtl4a', work / f'key.{arch}'
        if dst.is_file() and kf.is_file() and kf.read_text().strip() == key: print(f'pso_offline: {arch}: reused ({dst.stat().st_size / 2**20:.0f} MB)'); continue
        kf.unlink(missing_ok=True); dst.unlink(missing_ok=True)
        part = work / f'{arch}.mtl4a.partial'; part.unlink(missing_ok=True)
        cmd = ['xcrun', '-sdk', sdk(a.platform), 'metal-tt', '-arch', arch, f'-{a.platform}-version-min', osmin, str(js), '-o', str(part)]
        t1 = time.time(); print(f'pso_offline: {arch}: metal-tt, {len(bases)} bases (minutes) ...', flush=True)
        with open(work / f'metal-tt.{arch}.log', 'w') as log:
            r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, TMPDIR=str(work)))   # its temporary files on the work folder's volume
        if r.returncode or not part.is_file():
            sys.exit(f'[RED] pso_offline: metal-tt failed for {arch} (exit {r.returncode}): {(work / f"metal-tt.{arch}.log").read_text()[-2000:]}')
        os.replace(part, dst); kf.write_text(key + '\n')
        print(f'pso_offline: {arch}: {dst.stat().st_size / 2**20:.0f} MB in {time.time() - t1:.0f} s')
    for f in out.glob('*.mtl4a'):   # architectures no longer asked for
        if f.stem not in archs: f.unlink()
    (out / 'prebuilt.tsv').write_text(st)
    rp = out / 'rest_pairs.tsv'
    if rest: rp.write_text(''.join(f'{v}\t{f}\n' for v, f in rest))
    else: rp.unlink(missing_ok=True)
    (out / 'archive.tsv').write_text(f'# the pipeline archive made offline (dm/gfx/tools/pso_offline.py); the host reads prebuilt.tsv, not this file\narch\t{" ".join(archs)}\n'
                                     f'platform\t{a.platform}\t{osmin}\ntoolchain\t{tv}\nbases\t{len(bases)}\nrest\t{len(rest)}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
