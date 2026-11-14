#!/usr/bin/env python3
"""Evidence that the extracted RomFS is the base game with update 1.0.1: what the update changed over the base game alone, and the shaders against nxnative's table.
The shader split runs here on both extractions, outside the checkout; the table nxnative made from its own copy of 1.0.1 is the outside reference (assets/expected-1.0.1.json)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from inspect_inputs import EXTERNAL, TITLE
from ss_env import environment
from verify_ss import set_digest

GAME = Path(__file__).resolve().parents[1]


def tree(root):
    out = {}
    for p in sorted(root.rglob('*')):
        if p.is_file() and p.name != '.DS_Store':
            with p.open('rb') as stream:
                out[p.relative_to(root).as_posix()] = (p.stat().st_size, hashlib.file_digest(stream, 'sha256').hexdigest())
    return out


def compare(base, merged):
    """Files of the merged RomFS against the base game's: the counts, and every file the update touched."""
    changed = sorted(k for k in base.keys() & merged.keys() if base[k] != merged[k])
    added, removed = sorted(merged.keys() - base.keys()), sorted(base.keys() - merged.keys())
    return dict(same=len(base.keys() & merged.keys())-len(changed), changed=changed, added=added, removed=removed)


def split(root, out, env):
    out.mkdir(parents=True)
    table = out/'shader_map.tsv'
    subprocess.run([sys.executable, str(GAME/'runtime/gfx/tools/shader_split.py'), str(root/'RomFS'), str(root/'ExeFS/main'), str(out/'pkg'), str(table),
                    str(out/'shader_facts.tsv'), str(out/'shader_pairs.tsv')], env=env, check=True, stdout=subprocess.DEVNULL)
    return set_digest(table)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--merged', required=True, type=Path)
    parser.add_argument('--base', required=True, type=Path, help='the base game alone, from extract_ss.py without --update')
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path, help='new folder')
    args = parser.parse_args()
    evidence = args.evidence.expanduser().resolve()
    if evidence.exists():
        parser.error('evidence must be a new folder')
    env = environment(args.work_dir.expanduser().resolve(), TITLE)
    merged, base = args.merged.expanduser().resolve(), args.base.expanduser().resolve()
    diff = compare(tree(base/'RomFS'), tree(merged/'RomFS'))
    digest, keys = split(merged, evidence/'merged', env)
    base_digest, base_keys = split(base, evidence/'base', env)
    touched = len(diff['changed'])+len(diff['added'])+len(diff['removed'])
    match = digest == EXTERNAL['shader_stage_key_sha256'] and keys == EXTERNAL['shader_keys']
    result = dict(status='pass' if touched and match else 'fail', romfs=diff, shader_keys=keys, shader_digest=digest, matches_nxnative=match,
                  base_shader_keys=base_keys, base_shader_digest=base_digest, shaders_tell_base_from_update=base_digest != digest)
    (evidence/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'romfs'}|{'same': diff['same'], 'touched': touched}))
    return 0 if result['status'] == 'pass' else 1


if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error: sys.exit(f'error: {error}')
