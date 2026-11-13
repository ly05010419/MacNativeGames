#!/usr/bin/env python3
"""Build LM3's portable, verified schema-1 shader seed from local caches.

Only translated MSL, reflection, variant indexes and portable warm-up rows are
published. Input caches are read-only. No game save, code/SPIR-V intermediate,
device library or machine-specific Metal driver cache is copied.
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CHAIN_HEADER = ROOT / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
KEY = re.compile(r'[0-9a-f]{64}')
VARIANT = re.compile(r'([0-9a-f]{64})-[0-9a-f]{12}')
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
STAGES = {'vs': 0, 'ps': 1, 'cs': 5}
# nvn_pso_skey.h's arm64 disk ABI. The unit test checks these offsets against
# the actual C header so a renderer layout change cannot silently ship old rows.
PSO_SKEY_BYTES = 544
PSO_KEY_OFFSET = 200
PSO_POINTER_OFFSETS = (200, 208, 536)


def current_chain() -> str:
    match = re.search(r'^#define DMSH_CHAIN "([^"\n]+)"$', CHAIN_HEADER.read_text(), re.MULTILINE)
    if not match:
        raise ValueError('cannot read LM3 shader translation chain')
    return match[1]


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False,
                       allow_nan=False) + '\n').encode('utf-8')


def read_regular(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'expected a regular shader file: {path}')
    return path.read_bytes()


def read_json(path: Path) -> object:
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f'duplicate JSON key {key!r}')
            result[key] = value
        return result

    def constant(value):
        raise ValueError(f'non-finite JSON value {value}')

    try:
        return json.loads(read_regular(path), object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, ValueError) as error:
        raise ValueError(f'invalid shader JSON {path}: {error}') from error


def validate_deps(value: object, source: Path) -> dict:
    if not isinstance(value, dict) or set(value) != {'tex', 'img', 'attr', 'fout'}:
        raise ValueError(f'invalid dependency kinds: {source}')
    for kind, slots in value.items():
        if not isinstance(slots, dict):
            raise ValueError(f'invalid dependency slots: {source}')
        for slot, answer in slots.items():
            if not re.fullmatch(r'0|[1-9][0-9]*', slot):
                raise ValueError(f'invalid dependency slot: {source}')
            if kind == 'img':
                if not isinstance(answer, dict) or set(answer) - {'type', 'fmt'} or any(
                        not isinstance(item, str) or not item or not re.fullmatch(r'[A-Za-z0-9_]+', item)
                        for item in answer.values()):
                    raise ValueError(f'invalid image dependency: {source}')
            elif kind in {'attr', 'fout'}:
                if not isinstance(answer, str) or answer not in {'float', 'sint', 'uint'}:
                    raise ValueError(f'invalid numeric dependency: {source}')
            elif not isinstance(answer, str) or not re.fullmatch(r'[a-z0-9_]+', answer):
                raise ValueError(f'invalid texture dependency: {source}')
    return value


def load_caches(caches: list[Path], chain: str) -> tuple[dict[str, dict], dict[str, bytes]]:
    variants: dict[str, dict] = {}
    payload: dict[str, bytes] = {}
    for cache in caches:
        for index in sorted(cache.glob('*.variants')):
            shader = index.name.removesuffix('.variants')
            if not KEY.fullmatch(shader):
                raise ValueError(f'invalid variant index filename: {index}')
            rows = read_json(index)
            if not isinstance(rows, list):
                raise ValueError(f'variant index is not an array: {index}')
            for row in rows:
                if not isinstance(row, dict) or set(row) != {'variant', 'deps'}:
                    raise ValueError(f'invalid variant index row: {index}')
                name = row['variant']
                match = VARIANT.fullmatch(name) if isinstance(name, str) else None
                if not match or match[1] != shader:
                    raise ValueError(f'variant does not belong to its shader index: {index}')
                deps = validate_deps(row['deps'], index)
                reflection_file = cache / (name + '_b.json')
                reflection = read_json(reflection_file)
                if not isinstance(reflection, dict) or reflection.get('chain') != chain:
                    raise ValueError(f'shader reflection chain mismatch: {reflection_file}')
                if reflection.get('key') != name + '_b' or reflection.get('route') != 'B':
                    raise ValueError(f'shader reflection identity mismatch: {reflection_file}')
                stage, entry = reflection.get('stage'), reflection.get('entry')
                if not isinstance(stage, str) or stage not in STAGES or not isinstance(entry, str) or not IDENTIFIER.fullmatch(entry):
                    raise ValueError(f'invalid shader reflection stage/entry: {reflection_file}')
                if validate_deps(reflection.get('deps'), reflection_file) != deps:
                    raise ValueError(f'shader reflection dependency mismatch: {reflection_file}')
                metal_file = cache / (name + '_b.metal')
                metal = read_regular(metal_file)
                try:
                    text = metal.decode('utf-8')
                except UnicodeError as error:
                    raise ValueError(f'invalid MSL encoding: {metal_file}') from error
                kind = {'vs': 'vertex', 'ps': 'fragment', 'cs': 'kernel'}[stage]
                if not text.strip() or '\0' in text or not re.search(r'\b' + kind + r'\b', text) or not re.search(
                        r'\b' + re.escape(entry) + r'\s*\(', text):
                    raise ValueError(f'MSL entry does not match reflection: {metal_file}')
                info = {'shader': shader, 'deps': deps, 'stage': stage}
                if name in variants and variants[name] != info:
                    raise ValueError(f'conflicting variant index metadata: {name}')
                variants[name] = info
                for filename, data in ((metal_file.name, metal), (reflection_file.name, json_bytes(reflection))):
                    if filename in payload and payload[filename] != data:
                        raise ValueError(f'conflicting shader contents across caches: {filename}')
                    payload[filename] = data
    if not variants:
        raise ValueError('no complete shader variants found in the input caches')
    indexes: dict[str, list] = {}
    for name, info in sorted(variants.items()):
        indexes.setdefault(info['shader'], []).append({'variant': name, 'deps': info['deps']})
    payload.update({name + '.variants': json_bytes(rows) for name, rows in indexes.items()})
    return variants, payload


def shader_reference(key: str, stage: int, variants: dict[str, dict], source: str) -> None:
    if not key.endswith('_b') or not VARIANT.fullmatch(key[:-2]) or key[:-2] not in variants:
        raise ValueError(f'PSO references a missing shader: {source}')
    if STAGES[variants[key[:-2]]['stage']] != stage:
        raise ValueError(f'PSO shader stage mismatch: {source}')


def render_row(value: str, variants: dict[str, dict], source: str) -> str:
    if not re.fullmatch(r'[0-9a-fA-F]{' + str(PSO_SKEY_BYTES * 2) + r'}', value):
        raise ValueError(f'invalid render PSO encoding or ABI size: {source}')
    data = bytes.fromhex(value)
    for offset in PSO_POINTER_OFFSETS:
        if any(data[offset:offset + 8]):
            raise ValueError(f'nonportable pointer in render PSO: {source}')
    if data[162] > 1 or data[163] > 1 or any(data[198:PSO_KEY_OFFSET]):
        raise ValueError(f'invalid render PSO flags/padding: {source}')
    for start, stage, required in ((0, data[160], True), (80, data[161], False)):
        field = data[start:start + 80]
        end = field.find(b'\0')
        if end < 0 or any(field[end:]):
            raise ValueError(f'invalid render PSO shader key field: {source}')
        if not end:
            if required or stage:
                raise ValueError(f'render PSO has no required shader: {source}')
            continue
        try:
            key = field[:end].decode('ascii')
        except UnicodeError as error:
            raise ValueError(f'invalid render PSO shader key: {source}') from error
        shader_reference(key, 0 if start == 0 else 1, variants, source)
        if stage != (0 if start == 0 else 1):
            raise ValueError(f'invalid render PSO shader stage: {source}')
    if data[163] and (data[164] != 1 or not data[80]):
        raise ValueError(f'unsupported render PSO specialization: {source}')
    return 'R\t' + value.lower()


def prepare_pso(paths: list[Path], variants: dict[str, dict]) -> bytes:
    # Include every prepared library in first-launch warm-up, including shaders
    # collected without a profile's S row. The cache remains useful if no PSO
    # lists have been recorded yet.
    rows = {f'S\t{name}_b\t{STAGES[info["stage"]]}' for name, info in variants.items()}
    for path in sorted(set(paths)):
        try:
            text = read_regular(path).decode('utf-8')
        except UnicodeError as error:
            raise ValueError(f'invalid PSO list encoding: {path}') from error
        for number, line in enumerate(text.splitlines(), 1):
            if not line or line.startswith('#'):
                continue
            where = f'{path}:{number}'
            columns = line.split('\t')
            if columns[0] == 'S' and len(columns) == 3 and columns[2] in {'0', '1', '5'}:
                shader_reference(columns[1], int(columns[2]), variants, where)
                rows.add(line)
            elif columns[0] == 'C' and len(columns) == 2:
                shader_reference(columns[1], 5, variants, where)
                rows.add(line)
            elif columns[0] == 'R' and len(columns) == 2:
                rows.add(render_row(columns[1], variants, where))
            else:
                raise ValueError(f'invalid PSO row: {where}')
    order = {'S': 0, 'C': 1, 'R': 2}
    return ('# LM3 portable shader/pipeline warm-up v1\n' +
            '\n'.join(sorted(rows, key=lambda line: (order[line[0]], line))) + '\n').encode('utf-8')


def exchange_directories(first: Path, second: Path) -> None:
    """Swap existing directories atomically; failure leaves the old seed intact."""
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'darwin':
        function = libc.renamex_np
        function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        arguments = (os.fsencode(first), os.fsencode(second), 2)  # RENAME_SWAP
    elif sys.platform.startswith('linux') and hasattr(libc, 'renameat2'):
        function = libc.renameat2
        function.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        arguments = (-100, os.fsencode(first), -100, os.fsencode(second), 2)  # RENAME_EXCHANGE
    else:
        raise OSError(errno.ENOTSUP, 'atomic directory exchange unavailable')
    function.restype = ctypes.c_int
    if function(*arguments):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(second))


def prepare_seed(caches: list[Path], pso_lists: list[Path], output: Path) -> dict:
    """Validate/merge caches, then atomically publish a deterministic seed.

    pso_collected.tsv and pso_manifest.tsv in each cache are included
    automatically; pso_lists supplies optional extra profile warm-up lists.
    """
    output = output.expanduser()
    if output.is_symlink():
        raise ValueError('shader seed output must not be a symlink')
    output = output.resolve()
    caches = sorted(set(path.expanduser().resolve() for path in caches))
    if not caches or any(not path.is_dir() for path in caches):
        raise ValueError('each shader cache must be an existing directory')
    for cache in caches:
        if output.is_relative_to(cache) or cache.is_relative_to(output):
            raise ValueError('shader seed output must not overlap an input cache')
    lists = [path.expanduser().resolve() for path in pso_lists]
    for cache in caches:
        lists.extend(cache / name for name in ('pso_collected.tsv', 'pso_manifest.tsv') if (cache / name).exists())
    if any(path.is_relative_to(output) for path in lists):
        raise ValueError('shader seed output must not contain an input PSO list')
    chain = current_chain()
    variants, payload = load_caches(caches, chain)
    payload['pso_manifest.tsv'] = prepare_pso(lists, variants)
    manifest = {'schema': 1, 'chain': chain, 'shaders': len(variants),
                'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())}}
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f'.{output.name}.stage-', dir=output.parent))
    try:
        for name, data in sorted(payload.items()):
            (staging / name).write_bytes(data)
        (staging / 'seed-manifest.json').write_bytes(json_bytes(manifest))
        if output.exists():
            if not output.is_dir() or output.is_symlink():
                raise ValueError('shader seed output is not a directory')
            exchange_directories(staging, output)
        else:
            staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', action='append', required=True, type=Path, help='read-only cache; repeat to merge')
    parser.add_argument('--pso-list', action='append', default=[], type=Path, help='additional warm-up list; repeat to merge')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = prepare_seed(args.cache, args.pso_list, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'shader seed preparation failed: {error}\n')
    print(json.dumps({'status': 'pass', 'schema': manifest['schema'], 'chain': manifest['chain'],
                      'shaders': manifest['shaders'], 'files': len(manifest['files']),
                      'output': str(args.output.expanduser().resolve())}, sort_keys=True))


if __name__ == '__main__':
    main()
