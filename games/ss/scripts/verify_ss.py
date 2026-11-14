#!/usr/bin/env python3
"""Checks of the build's results: the shader libraries and a rewrite that cut nothing (the profile has no patches.py)."""
import hashlib
import json
from pathlib import Path
import struct

import shader_origin

GAME = Path(__file__).resolve().parents[1]
SLOT_KINDS = {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf', 'ubo_ptr', 'sbuf_ubo', 'tex_ubo', 'samp_ubo', 'samp_id'}
OTHER_KINDS = {'geometry', 'support', 'c1', 'drvcb', 'tex_c2a', 'samp_c2a', 'border', 'gmem_log', 'gmem_map', 'samp_pool', 'tex_pool'}   # with nxnative b1dee89's kinds (dmsh_abi.py KINDS)
BUILD = '5441af3c89f97e1d25a8ccf14c742bf8'   # main of 1.0.1 (v65536)


def map_rows(shader_map):
    """{key: (package, stage)} of the rows from a shader package (a key is listed once)."""
    rows = {}
    for line in shader_map.read_text().splitlines():
        c = line.split('\t')
        if len(c) >= 4 and c[0].endswith('.bin'):
            rows[c[3]] = (c[0], c[2])
    return rows


def set_digest(shader_map):
    """The digest of the set of (stage, key), the same over a generated table and nxnative's (which has a fifth column)."""
    rows = {f'{stage}\t{key}\n' for key, (_, stage) in map_rows(shader_map).items()}
    return hashlib.sha256(''.join(sorted(rows)).encode()).hexdigest(), len(rows)


def check_exceptions(libraries, rows, exceptions, romfs):
    """{key: 'absent' | 'present'} of the excepted shaders. Evidence by field: the build's outcome from _b_build.json (key, st, err), package and stage from the generated map
    (written before translation), origin and stage from the game's file (shader_origin). Any other failed key, or an excepted key that is half there, stops the package."""
    status = {r['key']: r for r in json.loads((libraries/'_b_build.json').read_text())}
    if set(status) != set(rows):
        raise ValueError(f'shader outcomes differ from the map: {len(set(status) ^ set(rows))} keys')
    allowed = {e['key']: e for e in exceptions}
    other = sorted(k for k, r in status.items() if r['st'] != 'ok' and k not in allowed)
    if other:
        raise ValueError(f'shaders failed outside the exception: {other[:8]} ({len(other)})')
    states = {}
    for key, e in allowed.items():
        if key not in rows:
            raise ValueError(f'excepted shader not in the map: {key}')
        if rows[key] != (e['package'], e['stage']):
            raise ValueError(f'excepted shader row differs: {key} {rows[key]}')
        shader_origin.check(romfs, e)
        st, files = status[key]['st'], sorted(p.name for p in libraries.glob(key+'*'))
        if st == 'ok':
            states[key] = 'present'
        elif st != e['st']:
            raise ValueError(f'excepted shader failed another way: {key} {st}: {status[key]["err"]}')
        elif files:
            raise ValueError(f'excepted shader half built: {key} {files}')
        else:
            states[key] = 'absent'
    return states


def verify_libraries(libraries, shader_map, exceptions=(), romfs=None):
    """An offline package requires a real library and reflection for every key but the excepted ones proven absent."""
    rows = map_rows(shader_map)
    if not rows:
        raise ValueError('empty shader map')
    states = check_exceptions(libraries, rows, exceptions, romfs) if exceptions else {}
    keys = {k for k in rows if states.get(k) != 'absent'}
    for library in libraries.glob('*.metallib'):
        with library.open('rb') as stream:
            if stream.read(4) != b'MTLB':
                raise ValueError(f'not an offline Metal library: {library.name}')
    folded = []
    for key in sorted(keys):
        if not (libraries/f'{key}_b.json').is_file():
            raise ValueError(f'incomplete offline shader key: {key}')
        reflection = json.loads((libraries/f'{key}_b.json').read_text())
        if reflection.get('chain') != libraries.name.removeprefix('b-') or reflection.get('dm_ctl') != key:
            raise ValueError(f'stale shader reflection: {key}')
        for binding in reflection.get('bindings', []):
            if binding.get('kind') not in SLOT_KINDS | OTHER_KINDS:
                raise ValueError(f'unknown shader binding: {key} {binding.get("msl_name")}')
            if binding.get('kind') in SLOT_KINDS and (type(binding.get('reg')) is not int or binding['reg'] < 0):
                raise ValueError(f'unresolved shader binding: {key} {binding.get("msl_name")}')
        if reflection.get('drv_unfillable'):
            raise ValueError(f'unfillable driver reads: {key}')
        if reflection.get('stage') == 'gs' and reflection.get('gs_slice'):
            # Exact layer-only geometry lowering runs in the paired vertex library; it has no standalone Metal geometry function/library.
            layer = reflection['gs_slice']
            if layer.get('loc') not in range(32) or layer.get('comp') not in ('x', 'y', 'z', 'w'):
                raise ValueError(f'invalid folded geometry reflection: {key}')
            folded.append(key)
            continue
        if not (libraries/f'{key}_b.metallib').is_file():
            raise ValueError(f'incomplete offline shader key: {key}')
        for field, suffix in (('fetch', '_bf'), ('vin', '_bv')):
            if reflection.get(field) and not (libraries/f'{key}{suffix}.metallib').is_file():
                raise ValueError(f'missing {field} library: {key}')
        if reflection.get('stage') == 'gs':
            for suffix in ('_bg0', '_bg1', '_bg2', '_bg3', '_bg4', '_bgv'):
                if not (libraries/f'{key}{suffix}.metallib').is_file():
                    raise ValueError(f'missing geometry variant: {key}{suffix}')
    return {'keys': len(rows), 'exceptions': states, 'folded_layer_geometry': folded,
            'metallib_files': sum(1 for _ in libraries.glob('*.metallib'))}


def text_segment(path):
    """(memory address, bytes) of an NSO's text segment, read here and not through the rewriter."""
    import lz4.block
    b = path.read_bytes()
    if b[:4] != b'NSO0':
        raise ValueError(f'not an NSO: {path.name}')
    offset, address, size = struct.unpack_from('<III', b, 0x10)
    stored = struct.unpack_from('<I', b, 0x60)[0]
    raw = b[offset:offset+stored]
    if struct.unpack_from('<I', b, 12)[0] & 1:
        raw = lz4.block.decompress(raw, uncompressed_size=size)
    return address, raw


def verify_cut(original, patched):
    """No cut, from outside the rewriter: main is the 1.0.1 build and cuts.tsv lists no row (the profile has no patches.py)."""
    if original.read_bytes()[0x40:0x50].hex() != BUILD:
        raise ValueError('main is not the 1.0.1 build')
    rows = [l.split('\t')[:3] for l in (patched/'main.cuts.tsv').read_text().splitlines() if l and not l.startswith('#')]
    if rows:
        raise ValueError(f'unexpected cuts: {rows}')
    return dict(build=BUILD, cuts=0)
