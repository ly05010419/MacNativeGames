#!/usr/bin/env python3
"""LM3 v4 shader identifiers and portable pipeline recipes, never shader code.

merge validates metadata from an explicitly selected LM3 cache. It drops shader
states served by the default 2D/float bindings and names the other states after
their dependencies. export resolves those names against a selected cache before
writing runtime warm-up rows. This does not upload to the website shader inbox.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


CHAIN = 'totk-native-v4-unverified'
HEADER = ('# LM3 shader list v1\n'
          '# Identifiers and pipeline descriptors only; no shader code, game resources or personal data.\n'
          '# Game 0100DCA0064A6000; translation chain totk-native-v4-unverified; pso_skey ABI lm3-v4/544.\n')
KEY = re.compile(r'[0-9a-f]{64}\Z')
NAME = re.compile(r'([0-9a-f]{64})-([0-9a-f]{12})_b\Z')
DEFAULTS = {'tex': '2d', 'img': {}, 'attr': 'float', 'fout': 'float'}
STAGES = {'vs': 0, 'ps': 1, 'cs': 5}
SKEY_BYTES = 544
POINTER_OFFSETS = (200, 208, 536)
# pso_key/nvn_obj.h on arm64, including the LM3 SCREEN contribution's
# ten advanced-blend bits at 37..46. This does not imply runtime support.
BLEND_OFFSETS = range(272, 336, 8)
ATTR_OFFSETS = range(336, 400, 4)


def compact(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False)


def suffix(answers):
    return hashlib.sha256(compact(answers).encode()).hexdigest()[:12]


EMPTY = {kind: {} for kind in DEFAULTS}
DEFAULT_SUFFIX = suffix(EMPTY)


def regular_text(path):
    path = Path(path).expanduser()
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'expected a regular metadata file: {path}')
    return path.read_text(encoding='utf-8')


def parse_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate metadata field')
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('non-finite metadata')))


def dependencies(value):
    if not isinstance(value, dict) or set(value) - set(DEFAULTS):
        raise ValueError('invalid dependency kinds')
    result = {kind: value.get(kind, {}) for kind in DEFAULTS}
    for kind, slots in result.items():
        if not isinstance(slots, dict):
            raise ValueError('invalid dependency slots')
        for slot, answer in slots.items():
            if not re.fullmatch(r'0|[1-9][0-9]{0,4}', slot) or int(slot) > 65535:
                raise ValueError('invalid dependency slot')
            if kind in ('attr', 'fout'):
                valid = isinstance(answer, str) and answer in ('float', 'sint', 'uint')
            elif kind == 'tex':
                valid = isinstance(answer, str) and re.fullmatch(r'[a-z0-9_]{1,40}', answer)
            else:
                valid = isinstance(answer, dict) and not (set(answer) - {'type', 'fmt'}) and all(
                    isinstance(item, str) and re.fullmatch(r'[A-Za-z0-9_]{1,40}', item)
                    for item in answer.values())
            if not valid:
                raise ValueError('invalid dependency answer')
    return result


def serves(deps, answers):
    """The LM3 v4 runtime compares all four kinds, including attr and fout."""
    return all(all(answers.get(kind, {}).get(slot, DEFAULTS[kind]) == wanted
                   for slot, wanted in slots.items()) for kind, slots in deps.items())


def canonical_name(shader, deps):
    answers = EMPTY if serves(deps, EMPTY) else deps
    return f'{shader}-{suffix(answers)}_b'


def pipeline(text):
    if not re.fullmatch(r'[0-9a-f]{1088}', text):
        raise ValueError('render recipe must use the LM3 v4 544-byte ABI')
    data = bytearray.fromhex(text)
    if any(any(data[offset:offset + 8]) for offset in POINTER_OFFSETS):
        raise ValueError('render recipe contains a process pointer')
    if data[162] > 1 or data[163] > 1 or any(data[198:200]):
        raise ValueError('invalid render flags or padding')
    if data[163] and data[164] != 1:
        raise ValueError('unsupported render specialization')
    for offset in BLEND_OFFSETS:
        word = int.from_bytes(data[offset:offset + 8], 'little')
        advanced, overlap = (word >> 37) & 63, (word >> 44) & 3
        if word >> 47 or advanced not in (0, *range(5, 51)) or overlap > 2:
            raise ValueError('invalid blend reserved bits or advanced enum')
    for offset in ATTR_OFFSETS:
        word = int.from_bytes(data[offset:offset + 4], 'little')
        if word & 255 and word >> 27 >= 16:
            raise ValueError('active vertex attribute has an out-of-bounds stream')
    if any(value >= 8 and value != 255 for value in data[528:536]):
        raise ValueError('out-of-bounds render attachment mapping')
    if not data[162] and any(data[528:536]):
        raise ValueError('unmerged render recipe has an attachment mapping')
    descriptor = data[164:198]
    if descriptor[0] == 0:
        if any(descriptor):
            raise ValueError('inactive render specialization has nonzero fields')
    elif descriptor[0] == 1 and data[162]:
        mask = 0
        for value in descriptor[2:]:
            if value == 255:  # int8_t -1: this texture is not an attachment.
                continue
            if value >= 8:
                raise ValueError('out-of-bounds render specialization attachment')
            mask |= 1 << value
        if mask != descriptor[1]:
            raise ValueError('render specialization attachment mask differs')
    else:
        raise ValueError('unsupported render specialization descriptor')
    names = []
    for start, expected in ((0, 0), (80, 1)):
        field = data[start:start + 80]
        end = field.find(b'\0')
        if end < 0 or any(field[end:]):
            raise ValueError('invalid render shader-name field')
        name = field[:end].decode('ascii')
        if not name:
            if start == 0 or data[160 + start // 80]:
                raise ValueError('missing required render shader')
        elif not NAME.fullmatch(name) or data[160 + start // 80] != expected:
            raise ValueError('invalid render shader identity/stage')
        names.append(name)
    if data[163] and not names[1]:
        raise ValueError('render specialization requires a fragment shader')
    return data, names


def rewrite_render(text, rename):
    data, names = pipeline(text)
    for start, name in zip((0, 80), names):
        if name:
            replacement = rename(name)
            if not NAME.fullmatch(replacement):
                raise ValueError('invalid resolved shader name')
            data[start:start + 80] = replacement.encode().ljust(80, b'\0')
    return data.hex()


def warm_row(fields):
    kind = fields[0] if fields else ''
    if kind == 'S' and len(fields) == 3 and NAME.fullmatch(fields[1]) and fields[2] in ('0', '1', '5'):
        return tuple(fields)
    if kind == 'C' and len(fields) == 2 and NAME.fullmatch(fields[1]):
        return tuple(fields)
    if kind == 'R' and len(fields) == 2:
        pipeline(fields[1])
        return tuple(fields)
    raise ValueError('unsupported warm-up row')


def references(row):
    if row[0] == 'R':
        return [(name, stage) for name, stage in zip(pipeline(row[1])[1], (0, 1)) if name]
    return [(row[1], int(row[2]) if row[0] == 'S' else 5)]


def flags_union(first, second):
    return ''.join(flag for flag in 'tm' if flag in first + second)


class ShaderList:
    def __init__(self):
        self.variants = set()
        self.answers = {}
        self.warm = {}

    def add_variant(self, shader, deps):
        if not KEY.fullmatch(shader):
            raise ValueError('invalid shader key')
        deps = dependencies(deps)
        if serves(deps, EMPTY):
            return False
        text = compact(deps)
        self.variants.add((shader, text))
        self.answers[suffix(deps)] = text
        return True

    def validate(self):
        known = {canonical_name(shader, parse_json(text)) for shader, text in self.variants}
        stages = {}
        for row in self.warm:
            for name, stage in references(row):
                match = NAME.fullmatch(name)
                if not match or (match[2] != DEFAULT_SUFFIX and name not in known):
                    raise ValueError('warm recipe has no matching non-default dependency row')
                if name in stages and stages[name] != stage:
                    raise ValueError('one shader has inconsistent stages')
                stages[name] = stage
        needed = {name.rsplit('-', 1)[1][:-2] for name in known}
        if set(self.answers) != needed:
            raise ValueError('binding-state rows do not match the shader variants')

    @classmethod
    def load(cls, path):
        result = cls()
        text = regular_text(path)
        if not text.startswith(HEADER):
            raise ValueError('not an LM3 v4 shader list')
        rows = [line.split('\t') for line in text.splitlines() if line and not line.startswith('#')]
        declared = {}
        for fields in rows:
            if fields[0] == 'V' and len(fields) == 3:
                deps = dependencies(parse_json(fields[2]))
                if compact(deps) != fields[2] or not result.add_variant(fields[1], deps):
                    raise ValueError('noncanonical or default-equivalent shader row')
            elif fields[0] == 'A' and len(fields) == 3:
                answers = dependencies(parse_json(fields[2]))
                if compact(answers) != fields[2] or suffix(answers) != fields[1] or fields[1] == DEFAULT_SUFFIX:
                    raise ValueError('invalid binding-state row')
                declared[fields[1]] = fields[2]
        if declared != result.answers:
            raise ValueError('missing or inconsistent binding-state rows')
        for fields in rows:
            if fields[0] in ('V', 'A'):
                if len(fields) != 3:
                    raise ValueError('invalid identifier row')
                continue
            if fields[-1] not in ('t', 'm', 'tm'):
                raise ValueError('invalid warm-up flags')
            row = warm_row(fields[:-1])
            result.warm[row] = flags_union(result.warm.get(row, ''), fields[-1])
        result.validate()
        return result

    def serialize(self):
        self.validate()
        lines = [f'V\t{shader}\t{text}' for shader, text in sorted(self.variants)]
        lines += [f'A\t{key}\t{text}' for key, text in sorted(self.answers.items())]
        lines += ['\t'.join(row) + '\t' + self.warm[row] for row in sorted(self.warm)]
        return HEADER + ''.join(line + '\n' for line in lines)

    def stats(self):
        self.validate()
        return {'V': len(self.variants), 'A': len(self.answers), **dict(Counter(row[0] for row in self.warm)),
                'shader_keys_with_nondefault_states': len({key for key, _ in self.variants}),
                'chain': CHAIN, 'pipeline_abi': 'lm3-v4/544'}


def load_caches(paths):
    """Read metadata only; no MSL, SPIR-V, native shader code or saved game."""
    cache = {}
    raw = {}
    for directory in sorted(set(Path(path).expanduser().resolve() for path in paths)):
        if not directory.is_dir():
            raise ValueError('shader cache does not exist')
        for index in sorted(directory.glob('*.variants')):
            shader = index.stem
            if not KEY.fullmatch(shader):
                raise ValueError('invalid variant index name')
            rows = parse_json(regular_text(index))
            if not isinstance(rows, list):
                raise ValueError('invalid variant index')
            for row in rows:
                if not isinstance(row, dict) or set(row) != {'variant', 'deps'}:
                    raise ValueError('invalid variant index row')
                name = row['variant'] + '_b' if isinstance(row['variant'], str) else ''
                match = NAME.fullmatch(name)
                if not match or match[1] != shader:
                    raise ValueError('variant belongs to another shader')
                deps = dependencies(row['deps'])
                reflection = parse_json(regular_text(directory / (name + '.json')))
                if not isinstance(reflection, dict) or reflection.get('chain') != CHAIN:
                    raise ValueError('shader metadata translation-chain mismatch')
                stage = reflection.get('stage')
                if reflection.get('key') != name or reflection.get('route') != 'B' or not isinstance(stage, str) or stage not in STAGES:
                    raise ValueError('shader metadata identity/stage mismatch')
                if dependencies(reflection.get('deps')) != deps:
                    raise ValueError('shader metadata dependencies differ from index')
                item = {'shader': shader, 'deps': deps, 'stage': STAGES[reflection['stage']]}
                if name in raw and raw[name] != item:
                    raise ValueError('conflicting variant metadata across caches')
                raw[name] = item
                cache.setdefault(shader, {})[name] = item
    if not raw:
        raise ValueError('no shader metadata found')
    return cache, raw


def add_cache_recipes(result, raw, paths):
    def rename(name):
        if name not in raw:
            raise ValueError('pipeline references a shader missing from selected LM3 caches')
        return canonical_name(raw[name]['shader'], raw[name]['deps'])
    for path in sorted(set(Path(path).expanduser().resolve() for path in paths)):
        for line in regular_text(path).splitlines():
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            row = warm_row(fields)
            for name, stage in references(row):
                if name not in raw or raw[name]['stage'] != stage:
                    raise ValueError('pipeline references a missing shader or the wrong stage')
            row = ('R', rewrite_render(row[1], rename)) if row[0] == 'R' else (row[0], rename(row[1]), *row[2:])
            result.warm[row] = flags_union(result.warm.get(row, ''), 'm')


def merge(existing, caches, pso_lists):
    existing = Path(existing).expanduser()
    result = ShaderList.load(existing) if Path(existing).exists() else ShaderList()
    _, raw = load_caches(caches)
    for item in raw.values():
        result.add_variant(item['shader'], item['deps'])
    recipes = list(pso_lists)
    for cache in caches:
        path = Path(cache).expanduser() / 'pso_collected.tsv'
        if path.exists():
            recipes.append(path)
    add_cache_recipes(result, raw, recipes)
    result.validate()
    return result


def export(result, caches):
    """Resolve portable names as the LM3 v4 runtime selects a cached variant."""
    cache, _ = load_caches(caches)
    variants = {canonical_name(key, parse_json(text)): parse_json(text) for key, text in result.variants}
    stages = {name: stage for row in result.warm for name, stage in references(row)}
    output, skipped = [], 0
    def resolve(name):
        key = NAME.fullmatch(name)[1]
        answers = variants.get(name, EMPTY)
        for raw, item in cache.get(key, {}).items():
            if item['stage'] == stages[name] and serves(item['deps'], answers):
                return raw
        raise LookupError(name)
    for row in sorted(result.warm):
        try:
            mapped = ('R', rewrite_render(row[1], resolve)) if row[0] == 'R' else (row[0], resolve(row[1]), *row[2:])
            output.append('\t'.join(mapped))
        except LookupError:
            skipped += 1
    return '# LM3 v4 warm-up identifiers resolved against a selected local cache.\n' + '\n'.join(output) + '\n', skipped


def atomic_text(path, text):
    path = Path(path).expanduser()
    if path.is_symlink():
        raise ValueError('output must not be a symlink')
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temp = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            handle.write(text)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def protect_inputs(output, caches, files=()):
    output = Path(output).expanduser().resolve()
    if any(output.is_relative_to(Path(path).expanduser().resolve()) for path in caches):
        raise ValueError('output overlaps a read-only shader cache')
    if any(output == Path(path).expanduser().resolve() for path in files):
        raise ValueError('output would replace an input metadata file')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    stats = commands.add_parser('stats'); stats.add_argument('list', type=Path)
    add = commands.add_parser('merge'); add.add_argument('list', type=Path)
    add.add_argument('--cache', action='append', type=Path, required=True)
    add.add_argument('--pso-list', action='append', type=Path, default=[])
    out = commands.add_parser('export'); out.add_argument('list', type=Path); out.add_argument('output', type=Path)
    out.add_argument('--cache', action='append', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'merge':
            protect_inputs(args.list, args.cache, args.pso_list)
            result = merge(args.list, args.cache, args.pso_list)
            atomic_text(args.list, result.serialize()); summary = result.stats()
        elif args.command == 'export':
            protect_inputs(args.output, args.cache, [args.list])
            result = ShaderList.load(args.list); text, skipped = export(result, args.cache)
            atomic_text(args.output, text); summary = {'exported_rows': len(text.splitlines()) - 1, 'unavailable_rows': skipped}
        else:
            summary = ShaderList.load(args.list).stats()
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f'LM3 shader identifiers: {error}\n')
    print(json.dumps(summary, sort_keys=True))


if __name__ == '__main__':
    main()
