#!/usr/bin/env python3
"""Research only: measure material-derived vertex variants missing from the seed.

Scans model metadata, not geometry. Uses existing seed reflection to identify
consumed inputs, but derives their types from material/mesh data. No production
seed/list/cache writes. Unsupported and texture-dependent cases stay explicit.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import time

from prepare_shader_seed import CHAIN, decoder
from read_material_metadata import body, external_strings, meshes, program_candidates
from shader_list import ShaderList, canonical


def deps_match(deps, answers):
    """Mirror totk_shader_rt.m, including defaults and asymmetric dependency sets."""
    defaults = dict(tex='2d', img={}, attr='float', fout='float')
    for kind, default in defaults.items():
        for slot, value in deps.get(kind, {}).items():
            if answers.get(kind, {}).get(slot, default) != value:
                return False
    return True


class Seed:
    def __init__(self, folder, romfs_hash):
        self.folder = folder
        self.manifest = json.loads((folder / 'seed-manifest.json').read_text())
        if self.manifest.get('chain') != CHAIN or self.manifest.get('source_romfs_sha256') != romfs_hash:
            raise ValueError('seed chain or source RomFS mismatch')
        self.indices = {}

    def checked_json(self, name):
        if Path(name).name != name:
            raise ValueError('unsafe seed file name')
        expected = self.manifest['files'].get(name)
        if not expected:
            raise ValueError('seed file missing from manifest')
        data = (self.folder / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('seed file integrity mismatch')
        return json.loads(data)

    def variants(self, shader):
        if shader not in self.indices:
            name = shader + '.variants'
            rows = self.checked_json(name) if name in self.manifest['files'] else []
            result = []
            for row in rows:
                reflection = self.checked_json(row['variant'] + '_b.json')
                if reflection.get('chain') != CHAIN or reflection.get('deps') != row['deps']:
                    raise ValueError('seed reflection/index mismatch')
                result.append(dict(**row, stage=reflection.get('stage')))
            self.indices[shader] = result
        return self.indices[shader]


def scan(romfs, seed_dir, shader_list, limit=None):
    index = json.loads((romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS did not pass integrity checks')
    entries = {row['path']: row for row in index['files']}
    seed = Seed(seed_dir, index['romfs_sha256'])
    shared = ShaderList.load(shader_list)
    listed = defaultdict(list)
    for key, deps in shared.variants:
        listed[key].append(json.loads(deps))
    counts, errors, unique, seen_contexts = Counter(), [], {}, set()
    archives = {}
    started = time.monotonic()
    files = sorted(p for p in entries if p.startswith('Model/') and p.endswith('.bfres.mc'))
    if limit is not None:
        files = files[:limit]
    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]; stream.seek(row['offset'])
            data = stream.read(row['size'])
            if len(data) != row['size']:
                raise ValueError('short RomFS read')
            return data
        decode = decoder(romfs, read)
        strings = external_strings(body(read('Shader/ExternalBinaryString.bfres.mc')))
        for file_index, path in enumerate(files):
            counts['files_attempted'] += 1
            try:
                parts = meshes(body(read(path)), strings)
            except (ValueError, KeyError, IndexError, UnicodeError) as error:
                counts['files_unsupported'] += 1
                errors.append(dict(path=path, stage='model', error=str(error)))
                continue
            counts['files_parsed'] += 1
            counts['meshes'] += len(parts)
            for mesh in parts:
                material = mesh['material']
                context = json.dumps(dict(archive=material['archive'], model=material['shader_model'],
                                          options=material['options'], render=material['render_info'],
                                          attributes=material['attributes'], vertex=mesh['vertex_attributes'],
                                          skin=mesh['skin_count']), sort_keys=True)
                if context in seen_contexts:
                    counts['duplicate_mesh_contexts'] += 1
                    continue
                seen_contexts.add(context)
                counts['unique_mesh_contexts'] += 1
                try:
                    name = material['archive']
                    if name not in archives:
                        paths = [p for p in entries if p.startswith(f'Shader/{name}.') and p.endswith('.bfsha.zs')]
                        if len(paths) != 1:
                            raise ValueError('missing or ambiguous top-level shader archive')
                        archives[name] = dict(data=decode(read(paths[0])), memo={})
                    archive = archives[name]
                    candidate = program_candidates(archive['data'], mesh, archive['memo'])
                    counts['candidate_programs'] += len(candidate['candidates'])
                    if not candidate['candidates']:
                        counts['contexts_without_programs'] += 1
                    answers = dict(attr=candidate['vertex_answers'], tex={}, img={}, fout={})
                    for program in candidate['candidates']:
                        shader = program['shader']
                        rows = seed.variants(shader)
                        if not rows:
                            counts['candidate_programs_without_seed_baseline'] += 1
                            continue
                        baselines = [row for row in rows if row['stage'] == 'vs' and
                                     not any(v for k, v in row['deps'].items() if k != 'attr')]
                        if not baselines:
                            counts['candidate_programs_with_unresolved_resources'] += 1
                            continue
                        for baseline in baselines:
                            required = baseline['deps'].get('attr', {})
                            if any(k not in candidate['vertex_answers'] for k in required):
                                counts['candidate_programs_with_unknown_vertex_input'] += 1
                                continue
                            deps = dict(tex={}, img={}, attr={k: candidate['vertex_answers'][k] for k in required}, fout={})
                            identity = (shader, canonical(deps))
                            if identity in unique:
                                unique[identity]['material_uses'] += 1
                                continue
                            usable = [r['variant'] for r in rows if deps_match(r['deps'], answers)]
                            in_list = (shader, identity[1]) in shared.variants
                            list_usable = any(deps_match(r, answers) for r in listed[shader])
                            unique[identity] = dict(shader=shader, dependencies=deps, answers=answers,
                                                    listed_exactly=in_list, listed_usable=list_usable,
                                                    prepared_usable=bool(usable), prepared_variant=usable[:1],
                                                    material_uses=1, model_path=path, shape=mesh['shape'],
                                                    material=material['name'], shader_archive=name,
                                                    shader_model=material['shader_model'], program_index=program['index'],
                                                    dynamic_choices=program['dynamic_choices'])
                except (ValueError, KeyError, IndexError, UnicodeError) as error:
                    counts['contexts_unsupported'] += 1
                    errors.append(dict(path=path, shape=mesh['shape'], stage='shader', error=str(error)))
            if (file_index + 1) % 100 == 0:
                print(f"models {file_index + 1}/{len(files)}; unique inferred {len(unique)}; "
                      f"missing {sum(not r['prepared_usable'] for r in unique.values())}; "
                      f"{time.monotonic() - started:.1f}s", flush=True)
    rows = list(unique.values())
    counts['unique_inferred_variants'] = len(rows)
    counts['already_usable_in_seed'] = sum(r['prepared_usable'] for r in rows)
    counts['missing_from_seed'] = sum(not r['prepared_usable'] for r in rows)
    counts['missing_from_seed_and_list'] = sum(not r['prepared_usable'] and not r['listed_usable'] for r in rows)
    return dict(chain=CHAIN, counts=dict(counts), errors=errors, variants=rows,
                seconds=round(time.monotonic() - started, 2),
                limitations=['Vertex shaders only; candidate passes are not observed draws.',
                             'Consumed-input sets come from verified seed reflection; types come from model metadata.',
                             'New candidates require fresh translation to confirm the predicted dependency dictionary.',
                             'Texture/image/output-dependent cases are not inferred.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--shader-list', type=Path, default=Path(__file__).resolve().parents[1] / 'assets/shader_list.tsv')
    parser.add_argument('--limit', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = scan(args.romfs, args.seed, args.shader_list, args.limit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['counts']), flush=True)
