"""Prepare TMNT NVN shaders from pinned MonoGame effects and embedded MGFX blobs.

Only shader identifiers / binding states are checked in. Shader bytes and translated
MSL stay in the player's external work folder. Runtime hashes do not name this cache.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

from rewrite_modules import GAME, sha256
from tmnt_bundle import Digests


def chain():
    return re.search(r'#define DMSH_CHAIN "([^"]+)"', (GAME / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text())[1]


def translation_sources():
    """Runtime rebuilds do not invalidate translations; translator/pin changes do."""
    from tools.deps.pins import SOURCES
    files = {}
    for folder in (GAME/'tools/totk_xlate', GAME/'third_party/ryujinx'):
        for path in sorted(folder.rglob('*')):
            if path.is_file() and path.suffix in ('.cs', '.csproj', '.cpp', '.h', '.props') and not {'obj','bin'}.intersection(path.relative_to(folder).parts):
                files[str(path.relative_to(GAME))] = sha256(path)
    files['spirv-cross-pin'] = SOURCES['spirv-cross']
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def shaders(data):
    """MGFX stores a sized 0x880-byte NVN control followed by a sized code buffer."""
    at = 0
    while True:
        at = data.find(b'\x34\x12\x76\x98', at)
        if at < 0:
            return
        pos = at; at += 4
        if pos < 4 or pos + 0x884 > len(data):
            continue
        control = data[pos:pos + 0x880]
        if struct.unpack_from('<I', data, pos - 4)[0] != 0x880 or struct.unpack_from('<I', control, 0x20)[0] != 0x880:
            continue
        constants, size = struct.unpack_from('<II', control, 0x700)
        stage = struct.unpack_from('<I', control, 0x714)[0]
        if stage not in (0, 1, 5) or not 0x30 <= constants <= size <= 16 << 20:
            continue
        if struct.unpack_from('<I', data, pos + 0x880)[0] != size or pos + 0x884 + size > len(data):
            raise ValueError('truncated MGFX shader code')
        code = data[pos + 0x884:pos + 0x884 + size]
        yield hashlib.sha256(control + code).hexdigest(), stage, control, code


def prepare(romfs, rewritten, output, translator, spirv_cross):
    output.mkdir(parents=True, exist_ok=True)
    digests = Digests(output.with_suffix('.digests.json'))
    index = json.loads((romfs / 'index.json').read_text())
    from expected_inputs import check_supported_romfs
    check_supported_romfs(index['romfs_sha256'])
    if digests.sha256(romfs / 'romfs.bin') != index['romfs_sha256']:
        raise ValueError('shader input RomFS changed')
    inventory = {}
    with (romfs / 'romfs.bin').open('rb') as stream:
        for row in index['files']:
            if not row['path'].startswith('Content/Shaders/') or not row['path'].endswith('.zxnb'):
                continue
            stream.seek(row['offset']); packed = stream.read(row['size'])
            unpacker = zlib.decompressobj(-15)
            raw = unpacker.decompress(packed, 16 << 20)
            if not unpacker.eof or unpacker.unused_data or not raw.startswith(b'XNBS'):
                raise ValueError('unsupported compressed TMNT effect')
            found = list(shaders(raw))
            if not found:
                raise ValueError(f'no NVN shaders in {row["path"]}')
            for key, stage, control, code in found: inventory[key] = stage, control, code
    for module in sorted(rewritten.glob('*.patched.bin')):
        for key, stage, control, code in shaders(module.read_bytes()): inventory[key] = stage, control, code
    if len(inventory) < 70:
        raise ValueError('TMNT shader inventory incomplete')
    from games.totk.scripts.shader_list import ShaderList
    shared = ShaderList.load(GAME / 'assets/shader_list.tsv')
    bindings = []
    for key, text in sorted(shared.variants):
        answers = dict(tex={}, img={}, attr={}, fout={}); answers.update(json.loads(text))
        answers['tex'] = {slot: {'*':'2d', '*_depth':'2d_depth'}.get(kind,kind) for slot,kind in answers['tex'].items()}
        bindings.append((key,answers))
    requests = [(key, {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}}) for key in sorted(inventory)]
    requests += [(key, answers) for key, answers in bindings]
    identity = {'schema': 1, 'version': 'v0', 'chain': chain(), 'translator': digests.sha256(translator),
                'spirv_cross': digests.sha256(spirv_cross), 'source_romfs_sha256': index['romfs_sha256'],
                'prepare': sha256(Path(__file__)), 'translation_sources': translation_sources(), 'bindings': sha256(GAME / 'assets/shader_list.tsv')}
    try:
        old = json.loads((output / 'seed-manifest.json').read_text())
        if all(old.get(k) == v for k, v in identity.items()) and all(digests.sha256(output / n) == h for n, h in old['files'].items()):
            return output
    except (OSError, ValueError, KeyError): pass
    records = {}
    def translate(job):
        key, answers = job
        if key not in inventory: raise ValueError(f'listed shader missing from pinned game: {key}')
        stage, control, code = inventory[key]
        suffix = hashlib.sha256(json.dumps(answers, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:12]
        variant = key + '-' + suffix
        code_path = output / (variant + '.code')
        code_path.write_bytes(code[0 if stage == 5 else 0x30:])
        constants = struct.unpack_from('<I', control, 0x700)[0]
        request = dict(variant=variant, stage={0:'vs', 1:'ps', 5:'cs'}[stage], code=str(code_path),
                       c1=code[constants:].hex(), depth_minus_w=True, **answers)
        if stage == 5: request['local'] = list(struct.unpack_from('<III', control, 0x748))
        result = subprocess.run([str(translator), '--service', '--spirv-cross', str(spirv_cross), '--out', str(output), '--chain', chain()],
                                input=json.dumps(request)+'\n', capture_output=True, text=True, check=True)
        response = json.loads(result.stdout.splitlines()[-1])
        if not response.get('ok'): raise ValueError(f'shader translation failed: {key}: {response}')
        reflection = json.loads((output / (variant + '_b.json')).read_text())
        if reflection['chain'] != chain(): raise ValueError('wrong translation chain')
        code_path.unlink()
        return key, variant, stage, reflection['deps']
    # Duplicate requests produce the same files, so eliminate them before parallel work.
    unique = list({(k, json.dumps(a, sort_keys=True)): (k,a) for k,a in requests}.values())
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(translate, unique))
    rows=[]; files=set()
    for key, variant, stage, deps in results:
        records.setdefault(key, []).append(dict(variant=variant, deps=deps))
        rows.append(f'S\t{variant}_b\t{stage}')
        files.update((variant+'_b.metal', variant+'_b.json'))
    for key, variants in records.items():
        (output / (key+'.variants')).write_text(json.dumps(variants, sort_keys=True)+'\n'); files.add(key+'.variants')
    (output / 'pso_manifest.tsv').write_text('\n'.join(sorted(set(rows)))+'\n'); files.add('pso_manifest.tsv')
    (output / 'shader-keys.json').write_text(json.dumps({'schema':1,'chain':chain(),'version':'v0','source_romfs_sha256':index['romfs_sha256'],'keys':sorted(inventory)})+'\n'); files.add('shader-keys.json')
    report = dict(identity, shaders=len(results), all_default=False, files={n:digests.sha256(output/n) for n in sorted(files)})
    (output / 'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n'); digests.save()
    return output
