#!/usr/bin/env python3
"""Offline BFRES/BFSHA to cached vertex-dependency experiment (no game launch).

Reads game data and cache, writes only a separate JSON report. Does not write shader
code, seed files, cache entries or app files. Format reference: MIT BfresLibrary,
Switch/Model/{MaterialParserV10,ModelParser,ShapeParser,VertexBufferParser}.cs.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path
import compression.zstd as zstd

from read_shader_archives import Binary, options
from prepare_shader_seed import CHAIN, ZSTD, control_key_len, suffix


class ResBinary(Binary):
    def __init__(self, data, strings=None):
        super().__init__(data)
        self.strings = strings or {}

    def string(self, at):
        return self.strings[at] if at in self.strings else super().string(at)


def body(raw):
    if raw[:4] != b'MCPK':
        raise ValueError('expected MCPK')
    data = zstd.ZstdDecompressor().decompress(ZSTD + raw[12:], max_length=64 << 20)
    if data[:4] != b'FRES' or struct.unpack_from('<H', data, 10)[0] != 10:
        raise ValueError('expected BFRES v10 metadata body')
    return data


def external_strings(data):
    b = Binary(data)
    names = b.dictionary(b.u64(0xc0))
    ids = b.unpack('Q' * len(names), b.u64(0xb8))
    if len(set(ids)) != len(ids):
        raise ValueError('duplicate external string IDs')
    return dict(zip(ids, names))


def indexed_assign(b, names, values, table, used, code):
    # V10 index tables have the compact forward indices first, then the inverse
    # indices used here (including -1 for defaults).
    indices = b.unpack(code * len(names), table + used * struct.calcsize('<' + code)) if table else range(len(names))
    result = {}
    for name, i in zip(names, indices):
        if i == -1:
            continue
        if i < 0 or i >= len(values):
            raise ValueError('assignment index outside compact values')
        result[name] = values[i]
    return result


def scalar_type(fmt):
    # Enumerated Switch BFRES formats, not a guess from the attribute name.
    uints = {0x0302, 0x020a, 0x0309, 0x0312, 0x030b, 0x090b, 0x0315, 0x0314,
             0x0317, 0x0318, 0x0319}
    sints = {0x0402, 0x040a, 0x0409, 0x0412, 0x040b, 0x099b, 0x0415, 0x0416,
             0x0417, 0x0418, 0x0419}
    floats = {0x0102, 0x0202, 0x0802, 0x0a02, 0x0001, 0x010a, 0x030a, 0x050a,
              0x0803, 0x0a03, 0x0109, 0x0209, 0x0804, 0x0a04, 0x0112, 0x0212,
              0x0512, 0x0807, 0x0a07, 0x0809, 0x010b, 0x020b, 0x080b, 0x0a0b,
              0x000b, 0x020e, 0x0115, 0x0215, 0x0515, 0x080e, 0x0a0e, 0x0516,
              0x0517, 0x0518, 0x0519}
    if fmt in uints:
        return 'uint'
    if fmt in sints:
        return 'sint'
    if fmt in floats:
        return 'float'
    raise ValueError(f'unsupported vertex format {fmt:#x}')


def material(b, at):
    if b.data[at:at + 4] != b'FMAT':
        raise ValueError('expected FMAT')
    info = b.u64(at + 16)
    assign = b.u64(info)
    attr_names = b.dictionary(b.u64(assign + 48))
    sampler_names = b.dictionary(b.u64(assign + 56))
    option_names = b.dictionary(b.u64(assign + 64))
    attr_count, sampler_count, bool_count, choice_count = b.unpack('BBHH', info + 68)
    def strings(ptr, count):
        return [b.string(b.u64(ptr + i * 8)) for i in range(count)]
    attr_values = strings(b.u64(info + 8), attr_count)
    sampler_values = strings(b.u64(info + 24), sampler_count)
    flags = b.unpack('Q' * (1 + bool_count // 64), b.u64(info + 40))
    choices = ['1' if flags[i // 64] & (1 << (i % 64)) else '0' for i in range(bool_count)]
    choices += strings(b.u64(info + 48), choice_count - bool_count)
    render_names = b.dictionary(b.u64(assign + 24))
    render_info = {}
    for i, name in enumerate(render_names):
        entry = b.u64(assign + 16) + i * 16
        if b.string(b.u64(entry)) != name:
            raise ValueError('render-info dictionary mismatch')
        typ = b.unpack('B', entry + 8)[0]
        count = b.unpack('H', b.u64(at + 72) + i * 2)[0]
        pos = b.u64(at + 64) + b.unpack('H', b.u64(at + 80) + i * 2)[0]
        if typ == 2:
            render_info[name] = strings(pos, count)
        elif typ in (0, 1):
            render_info[name] = list(b.unpack(('i' if typ == 0 else 'f') * count, pos))
        else:
            raise ValueError('unknown render-info type')
    tex_count = b.unpack('B', at + 163)[0]
    texture_names = strings(b.u64(at + 32), tex_count)
    physical_samplers = b.dictionary(b.u64(at + 56))
    if len(physical_samplers) != tex_count:
        raise ValueError('sampler/texture association needs an explicit non-1:1 mapping')
    return dict(name=b.string(b.u64(at + 8)), archive=b.string(b.u64(assign)),
                shader_model=b.string(b.u64(assign + 8)),
                attributes=indexed_assign(b, attr_names, attr_values, b.u64(info + 16), attr_count, 'b'),
                samplers=indexed_assign(b, sampler_names, sampler_values, b.u64(info + 32), sampler_count, 'b'),
                options=indexed_assign(b, option_names, choices, b.u64(info + 56), choice_count, 'h'),
                texture_names=texture_names, texture_bindings=dict(zip(physical_samplers, texture_names)),
                render_info=render_info)


def meshes(data, strings):
    b = ResBinary(data, strings)
    models = b.dictionary(b.u64(48))
    result = []
    for i, name in enumerate(models):
        at = b.u64(40) + i * 120
        if data[at:at + 4] != b'FMDL' or b.string(b.u64(at + 8)) != name:
            raise ValueError('invalid FMDL')
        mats = [material(b, b.u64(at + 56) + j * 176) for j in range(len(b.dictionary(b.u64(at + 64))))]
        for j, shape_name in enumerate(b.dictionary(b.u64(at + 48))):
            sh = b.u64(at + 40) + j * 96
            if data[sh:sh + 4] != b'FSHP':
                raise ValueError('expected FSHP')
            mat_index = b.unpack('H', sh + 82)[0]
            vb = b.u64(sh + 16)
            if data[vb:vb + 4] != b'FVTX':
                raise ValueError('expected FVTX')
            attrs = {}
            for k, aname in enumerate(b.dictionary(b.u64(vb + 16))):
                p = b.u64(vb + 8) + k * 16
                fmt = struct.unpack_from('>H', data, p + 8)[0]
                attrs[aname] = dict(format=hex(fmt), scalar=scalar_type(fmt))
            result.append(dict(model=name, shape=shape_name, skin_count=b.unpack('B', sh + 90)[0],
                               material=mats[mat_index], vertex_attributes=attrs))
    return result


def select_programs(keys, constraints, buckets):
    """Intersect compact bitsets; avoid scanning 23k programs for every mesh."""
    selected = (1 << len(keys)) - 1
    for word, mask, shift, choice in constraints:
        identity = (word, mask, shift)
        if identity not in buckets:
            groups = {}
            for i, key in enumerate(keys):
                value = (key[word] & mask) >> shift
                if value not in groups:
                    groups[value] = bytearray((len(keys) + 7) // 8)
                group = groups[value]
                group[i // 8] |= 1 << (i % 8)
            buckets[identity] = {v: int.from_bytes(group, 'little') for v, group in groups.items()}
        selected &= buckets[identity].get(choice, 0)
        if not selected:
            break
    result = []
    while selected:
        bit = selected & -selected
        result.append(bit.bit_length() - 1)
        selected ^= bit
    return result


def program_candidates(data, mesh, memo=None):
    """Only filter by known material/mesh settings; keep unknown pass choices explicit."""
    b = Binary(data)
    names = b.dictionary(b.u64(0x50))
    model_name = mesh['material']['shader_model']
    if model_name not in names:
        raise ValueError('material shader model absent from archive')
    at = b.u64(0x48) + names.index(model_name) * 0x100
    if memo is None:
        memo = {}
    if model_name not in memo:
        sw, dw = b.unpack('BB', at + 234)
        width = sw + dw
        count = b.unpack('H', at + 232)[0]
        memo[model_name] = dict(
            statics=options(b, b.u64(at + 8), b.dictionary(b.u64(at + 16))),
            dynamics=options(b, b.u64(at + 24), b.dictionary(b.u64(at + 32))),
            keys=[b.unpack('I' * width, b.u64(at + 144) + i * width * 4) for i in range(count)],
            buckets={}, selections={}, programs={})
    prepared = memo[model_name]
    statics, dynamics = prepared['statics'], prepared['dynamics']
    query = dict(mesh['material']['options'])
    query['gsys_weight'] = str(mesh['skin_count'])
    ri = mesh['material']['render_info']
    mode = ri.get('gsys_render_state_mode', [])
    if mode:
        query['gsys_renderstate'] = {'opaque': '0', 'mask': '1', 'translucent': '2', 'custom': '3'}[mode[0]]
    alpha = ri.get('gsys_alpha_test_enable', [])
    if alpha:
        query['gsys_alpha_test_enable'] = '1' if alpha[0] == 'true' else '0'
    # Compare-function mapping is not derived here: leave it unconstrained.
    constraints = []
    static_words, dynamic_words = b.unpack('BB', at + 234)
    for dynamic, group in ((False, statics), (True, dynamics)):
        for option in group:
            name = option['name']
            if name not in query:
                continue
            value = {'True': '1', 'False': '0'}.get(query[name], query[name])
            choices = list(option['choices'])
            if value not in choices:
                raise ValueError(f'unsupported material option choice for {name}')
            word = static_words + option['word'] - option['key_offset'] if dynamic else option['word']
            if not 0 <= word < static_words + dynamic_words:
                raise ValueError('option word outside key table')
            constraints.append((word, option['mask'], option['shift'], choices.index(value)))
    attr_array = b.u64(at + 40)
    inferred = {}
    unresolved_attributes = []
    for i, name in enumerate(b.dictionary(b.u64(at + 48))):
        location = b.unpack('b', attr_array + i * 2 + 1)[0]
        physical = mesh['material']['attributes'].get(name)
        found = mesh['vertex_attributes'].get(physical)
        if location >= 0 and found:
            inferred[str(location)] = found['scalar']
        elif location >= 0:
            unresolved_attributes.append(dict(name=name, location=location, assigned=physical))
    bnsh = b.u64(at + 168)
    programs = []
    signature = tuple(constraints)
    if signature not in prepared['selections']:
        prepared['selections'][signature] = select_programs(prepared['keys'], constraints, prepared['buckets'])
    for i in prepared['selections'][signature]:
        if i in prepared['programs']:
            programs.append(prepared['programs'][i])
            continue
        key = prepared['keys'][i]
        variation = b.u64(b.u64(at + 136) + i * 64 + 32)
        program = bnsh + b.u64(variation + 16)
        vertex = bnsh + b.u64(program + 8)
        control_at, code_at, code_size, control_size = b.unpack('QQII', vertex + 8)
        control = b.data[bnsh + control_at:bnsh + control_at + control_size]
        key_size = control_key_len(control)
        length = struct.unpack_from('<I', control, 0x704)[0]
        if not key_size or key_size > len(control) or length > code_size:
            raise ValueError('invalid vertex shader bounds')
        code = b.data[bnsh + code_at:bnsh + code_at + length]
        if len(code) != length:
            raise ValueError('short shader read')
        shader = hashlib.sha256(control[:key_size] + code).hexdigest()
        dynamic_choices = {}
        for option in dynamics:
            word = static_words + option['word'] - option['key_offset']
            choice = (key[word] & option['mask']) >> option['shift']
            choices = list(option['choices'])
            dynamic_choices[option['name']] = choices[choice] if choice < len(choices) else f'unknown:{choice}'
        record = dict(index=i, shader=shader, dynamic_choices=dynamic_choices)
        prepared['programs'][i] = record
        programs.append(record)
    return dict(known_constraints=len(constraints), vertex_answers=inferred,
                unresolved_attributes=unresolved_attributes, candidates=programs)


def compare_cache(candidate_report, cache):
    matches = []
    for shader in sorted({p['shader'] for p in candidate_report['candidates']}):
        index = cache / (shader + '.variants')
        if not index.is_file():
            continue
        for row in json.loads(index.read_text()):
            variant = row.get('variant', '')
            # A cache is input, not a trusted path provider.
            if len(variant) != 77 or not variant.startswith(shader + '-') or any(c not in '0123456789abcdef-' for c in variant):
                raise ValueError('invalid cache variant name')
            reflection_path = cache / (variant + '_b.json')
            if not reflection_path.is_file():
                continue
            reflection = json.loads(reflection_path.read_text())
            deps = reflection.get('deps')
            if reflection.get('stage') != 'vs' or deps != row.get('deps'):
                raise ValueError('cache index/reflection disagreement')
            inferred = candidate_report['vertex_answers']
            unknown = [k for k in deps.get('attr', {}) if k not in inferred]
            differences = {k: dict(inferred=inferred[k], cached=v) for k, v in deps.get('attr', {}).items()
                           if k in inferred and inferred[k] != v}
            other = {k: v for k, v in deps.items() if k != 'attr' and v}
            matches.append(dict(shader=shader, variant=variant, chain=reflection.get('chain'),
                                cached_dependencies=deps, unknown_vertex_inputs=unknown,
                                differing_vertex_inputs=differences, other_dependencies_not_inferred=other,
                                full_dependency_match=not unknown and not differences and not other))
    return matches


def translate_candidates(data, mesh, output, translator, spirv_cross):
    """Translate using material-derived answers only. The cache is never an input."""
    output.mkdir(parents=True, exist_ok=False)
    b = Binary(data)
    names = b.dictionary(b.u64(0x50))
    at = b.u64(0x48) + names.index(mesh['material']['shader_model']) * 0x100
    bnsh = b.u64(at + 168)
    inferred = mesh['offline_inference']
    answers = dict(tex={}, img={}, attr=inferred['vertex_answers'], fout={})
    requests, identities, paths = [], [], []
    seen = set()
    for candidate in inferred['candidates']:
        key = candidate['shader']
        if key in seen:
            continue
        seen.add(key)
        variation = b.u64(b.u64(at + 136) + candidate['index'] * 64 + 32)
        program = bnsh + b.u64(variation + 16)
        vertex = bnsh + b.u64(program + 8)
        ctl, code_at, code_size, ctl_size = b.unpack('QQII', vertex + 8)
        control = data[bnsh + ctl:bnsh + ctl + ctl_size]
        c1, length = struct.unpack_from('<II', control, 0x700)
        code = data[bnsh + code_at:bnsh + code_at + length]
        if not 0x30 <= c1 <= length <= code_size or len(code) != length:
            raise ValueError('invalid candidate code bounds')
        if hashlib.sha256(control[:control_key_len(control)] + code).hexdigest() != key:
            raise ValueError('candidate shader key mismatch')
        variant = key + '-' + suffix(answers)
        path = output / (variant + '.code')
        path.write_bytes(code[0x30:])
        paths.append(path)
        requests.append(dict(variant=variant, stage='vs', code=str(path.resolve()),
                             c1=code[c1:].hex(), depth_minus_w=True, **answers))
        identities.append(dict(shader=key, variant=variant))
    try:
        with (output / 'translator-stderr.log').open('w') as log:
            completed = subprocess.run([str(translator.resolve()), '--service', '--spirv-cross', str(spirv_cross.resolve()),
                                        '--out', str(output.resolve()), '--chain', CHAIN],
                                       input=''.join(json.dumps(r) + '\n' for r in requests), text=True,
                                       stdout=subprocess.PIPE, stderr=log, timeout=180)
        replies = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
        if completed.returncode or len(replies) != len(identities):
            raise ValueError('translation process failed or returned an unexpected reply count')
        results = []
        for identity, reply in zip(identities, replies):
            if not reply.get('ok'):
                results.append(dict(**identity, error=reply.get('error')))
                continue
            reflection = json.loads((output / (identity['variant'] + '_b.json')).read_text())
            if reflection.get('chain') != CHAIN or reflection.get('stage') != 'vs':
                raise ValueError('translation identity mismatch')
            deps = reflection['deps']
            covered = (all(inferred['vertex_answers'].get(k) == v for k, v in deps.get('attr', {}).items())
                       and not any(v for k, v in deps.items() if k != 'attr'))
            cache_matches = [m['variant'] for m in mesh.get('cache_comparison', [])
                             if m['shader'] == identity['shader'] and m['cached_dependencies'] == deps]
            results.append(dict(**identity, chain=CHAIN, dependencies=deps,
                                fully_material_derived=covered,
                                old_cache_exact_dependency_matches=cache_matches))
        return results
    finally:
        for path in paths:
            path.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--model', default='Model/Animal_Bass.Bass.bfres.mc')
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--translate-output', type=Path, help='new directory outside source/cache for isolated translation')
    parser.add_argument('--translator', type=Path)
    parser.add_argument('--spirv-cross', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.translate_output and not (args.translator and args.spirv_cross):
        parser.error('--translate-output requires --translator and --spirv-cross')
    if args.translate_output:
        target = args.translate_output.resolve()
        source = Path(__file__).resolve().parents[3]
        if target == source or source in target.parents or (args.cache and (target == args.cache.resolve() or args.cache.resolve() in target.parents)):
            parser.error('translation output must be outside source and cache')
    index = json.loads((args.romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity checks')
    entries = {e['path']: e for e in index['files']}
    with (args.romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            e = entries[name]; stream.seek(e['offset'])
            raw = stream.read(e['size'])
            if len(raw) != e['size']:
                raise ValueError('short RomFS read')
            return raw
        strings = external_strings(body(read('Shader/ExternalBinaryString.bfres.mc')))
        report = dict(path=args.model, meshes=meshes(body(read(args.model)), strings))
        from prepare_shader_seed import decoder
        decode = decoder(args.romfs, read)
        for mesh_index, mesh in enumerate(report['meshes']):
            name = mesh['material']['archive']
            archives = [p for p in entries if p.startswith(f'Shader/{name}.') and p.endswith('.bfsha.zs')]
            if len(archives) != 1:
                raise ValueError('ambiguous material archive')
            archive_data = decode(read(archives[0]))
            candidate_report = program_candidates(archive_data, mesh)
            mesh['offline_inference'] = candidate_report
            if args.cache:
                mesh['cache_comparison'] = compare_cache(candidate_report, args.cache)
            if args.translate_output:
                mesh['isolated_translation'] = translate_candidates(archive_data, mesh,
                    args.translate_output / f'mesh-{mesh_index}', args.translator, args.spirv_cross)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(meshes=len(report['meshes']), output=str(args.output))))
