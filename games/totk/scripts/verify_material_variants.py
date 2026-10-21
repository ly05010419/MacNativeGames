#!/usr/bin/env python3
"""Translate/compile missing material-derived candidates in a new external folder.

Never modifies the app, production seed, shared list or user cache. A generated
TSV contains identifiers/dependencies only, and is separate from upstream's list.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from prepare_shader_seed import CHAIN, control_key_len, decoder, suffix
from read_shader_archives import Binary
from find_material_variants import Seed, deps_match
from shader_list import ShaderList, canonical


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(report_path, romfs, seed_dir, list_path, translator, spirv_cross, output, metal=False):
    report = json.loads(report_path.read_text())
    if report['chain'] != CHAIN:
        raise ValueError('candidate report chain mismatch')
    index = json.loads((romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS did not pass integrity checks')
    seed = Seed(seed_dir, index['romfs_sha256'])
    if digest(translator) != seed.manifest['translator_sha256'] or digest(spirv_cross) != seed.manifest['spirv_cross_sha256']:
        raise ValueError('translation tools do not match the prepared seed')
    shared = ShaderList.load(list_path)
    entries = {r['path']: r for r in index['files']}
    selected = [r for r in report['variants'] if not r['prepared_usable']]
    output.mkdir(parents=True, exist_ok=False)
    requests, jobs, code_files = [], [], []
    archives = {}
    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            e = entries[name]; stream.seek(e['offset'])
            raw = stream.read(e['size'])
            if len(raw) != e['size']:
                raise ValueError('short RomFS read')
            return raw
        decode = decoder(romfs, read)
        for row in selected:
            name = row['shader_archive']
            if name not in archives:
                paths = [p for p in entries if p.startswith(f'Shader/{name}.') and p.endswith('.bfsha.zs')]
                if len(paths) != 1:
                    raise ValueError('ambiguous archive')
                archives[name] = decode(read(paths[0]))
            data = archives[name]
            b = Binary(data)
            models = b.dictionary(b.u64(0x50))
            at = b.u64(0x48) + models.index(row['shader_model']) * 0x100
            count = b.unpack('H', at + 232)[0]
            if not 0 <= row['program_index'] < count:
                raise ValueError('candidate program outside model')
            bnsh = b.u64(at + 168)
            variation = b.u64(b.u64(at + 136) + row['program_index'] * 64 + 32)
            program = bnsh + b.u64(variation + 16)
            vertex = bnsh + b.u64(program + 8)
            ctl, code_at, code_size, ctl_size = b.unpack('QQII', vertex + 8)
            control = data[bnsh + ctl:bnsh + ctl + ctl_size]
            c1, length = struct.unpack_from('<II', control, 0x700)
            code = data[bnsh + code_at:bnsh + code_at + length]
            if not 0x30 <= c1 <= length <= code_size or len(code) != length:
                raise ValueError('invalid shader code bounds')
            if hashlib.sha256(control[:control_key_len(control)] + code).hexdigest() != row['shader']:
                raise ValueError('candidate shader hash mismatch')
            variant = row['shader'] + '-' + suffix(row['answers'])
            if any(j['variant'] == variant for j in jobs):
                raise ValueError('duplicate translation request; candidate report needs deduplication')
            path = output / (variant + '.code')
            path.write_bytes(code[0x30:]); code_files.append(path)
            requests.append(dict(variant=variant, stage='vs', code=str(path.resolve()), c1=code[c1:].hex(),
                                 depth_minus_w=True, **row['answers']))
            jobs.append(dict(variant=variant, candidate=row))
    try:
        with (output / 'translator-stderr.log').open('w') as log:
            process = subprocess.run([str(translator.resolve()), '--service', '--spirv-cross', str(spirv_cross.resolve()),
                                      '--out', str(output.resolve()), '--chain', CHAIN],
                                     input=''.join(json.dumps(r) + '\n' for r in requests), text=True,
                                     stdout=subprocess.PIPE, stderr=log, timeout=1800)
        replies = [json.loads(line) for line in process.stdout.splitlines() if line.strip()]
        if process.returncode or len(replies) != len(jobs):
            raise ValueError('translator failed or reply count differs from requests')
        results = []
        for job, reply in zip(jobs, replies):
            variant, candidate = job['variant'], job['candidate']
            row = dict(shader=candidate['shader'], variant=variant, model_path=candidate['model_path'],
                       shape=candidate['shape'], dynamic_choices=candidate['dynamic_choices'])
            if not reply.get('ok'):
                results.append(dict(**row, error=reply.get('error')))
                continue
            reflection = json.loads((output / (variant + '_b.json')).read_text())
            if reflection.get('chain') != CHAIN or reflection.get('stage') != 'vs':
                raise ValueError('translation identity mismatch')
            deps = reflection['deps']
            derived = (all(candidate['answers']['attr'].get(k) == v for k, v in deps.get('attr', {}).items())
                       and not any(v for k, v in deps.items() if k != 'attr'))
            covered = any(deps_match(r['deps'], candidate['answers']) for r in seed.variants(candidate['shader']))
            results.append(dict(**row, dependencies=deps, fully_material_derived=derived,
                                matches_prediction=deps == candidate['dependencies'], already_usable_in_seed=covered,
                                listed_exactly=(candidate['shader'], canonical(deps)) in shared.variants))
        if metal:
            def compile_one(row):
                if 'error' in row:
                    return
                source = output / (row['variant'] + '_b.metal')
                compiled = source.with_suffix('.metallib')
                result = subprocess.run(['xcrun', 'metal', '-fmetal-math-mode=safe', '-o', str(compiled), str(source)],
                                        capture_output=True, text=True, timeout=180)
                row['metal_compiled'] = result.returncode == 0
                if result.returncode:
                    row['metal_error'] = result.stderr
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(compile_one, results))
        approved = {(r['shader'], canonical(r['dependencies'])) for r in results
                    if 'error' not in r and r['fully_material_derived'] and r['matches_prediction']
                    and not r['already_usable_in_seed'] and (not metal or r.get('metal_compiled'))}
        (output / 'offline-material-candidates.tsv').write_text(
            '# Offline material-derived vertex candidates, chain ' + CHAIN + '; not observed draws.\n' +
            ''.join(f'V\t{key}\t{deps}\n' for key, deps in sorted(approved)))
        summary = dict(requests=len(jobs), translated=sum('error' not in r for r in results),
                       matches_prediction=sum(r.get('matches_prediction', False) for r in results),
                       fully_material_derived=sum(r.get('fully_material_derived', False) for r in results),
                       metal_compiled=sum(r.get('metal_compiled', False) for r in results),
                       validated_additional_candidates=len(approved))
        result = dict(chain=CHAIN, summary=summary, results=results,
                      limitations=['Candidate passes, not proven actual draws.',
                                   'Covers only metadata-derived vertex inputs, not all shader stages/resources.'])
        (output / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
        return summary
    finally:
        for path in code_files:
            path.unlink(missing_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--shader-list', type=Path, default=Path(__file__).resolve().parents[1] / 'assets/shader_list.tsv')
    parser.add_argument('--translator', type=Path, required=True)
    parser.add_argument('--spirv-cross', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metal', action='store_true')
    args = parser.parse_args()
    target = args.output.resolve()
    source = Path(__file__).resolve().parents[3]
    forbidden = [source, args.seed.resolve(), args.romfs.resolve(),
                 (Path.home() / 'Library/Application Support/MacNativeGames/TOTK/shader-cache').resolve()]
    if any(target == p or p in target.parents for p in forbidden):
        parser.error('output must be a new folder outside source, seed, RomFS and cache')
    print(json.dumps(validate(args.report, args.romfs, args.seed, args.shader_list,
                              args.translator, args.spirv_cross, args.output, args.metal)), flush=True)
