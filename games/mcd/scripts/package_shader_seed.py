#!/usr/bin/env python3
"""Package only verified offline seed files and source-matched Metal libraries.

Previous shader resources (not Apps or game data) are retained in the work tree.
No save/profile/cache discovery or modification is performed.
"""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import uuid
from prepare_shader_seed import seed_files, sha


def package(seed, libraries, destination):
    files = seed_files(seed)
    selected = {}
    manifest = libraries/'itt-libraries.json'
    if manifest.exists():
        report = json.loads(manifest.read_text())
        if report.get('flags') != ['-fmetal-math-mode=safe']:
            raise ValueError('unexpected Metal compiler flags')
        sources = {digest for name, digest in files.items() if name.endswith('_b.metal')}
        for key, digest in report['libraries'].items():
            if key not in sources:
                continue  # old/orphaned libraries are never shipped
            src = libraries/(key+'.metallib')
            raw = src.read_bytes()
            if not raw.startswith(b'MTLB') or sha(raw) != digest:
                raise ValueError('Metal library digest mismatch: '+key)
            selected[src.name] = src
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mcd-shaders-', dir=destination.parent) as tmp:
        staging = Path(tmp)
        for name in ('shader-seed', 'shader-libs'):
            (staging/name).mkdir()
        for name in (*files, 'seed-manifest.json', 'offline-report.json'):
            shutil.copy2(seed/name, staging/'shader-seed'/name)
        for name, src in selected.items():
            shutil.copy2(src, staging/'shader-libs'/name)
        previous = seed.parent/'shader-package-previous'/uuid.uuid4().hex
        for name in ('shader-seed', 'shader-libs'):
            target = destination/name
            if target.is_symlink():
                raise ValueError('shader package target must not be a symlink')
        for name in ('shader-seed', 'shader-libs'):
            target = destination/name
            if target.exists():
                previous.mkdir(parents=True, exist_ok=True)
                # Build outputs and installed Apps can be on different volumes.
                shutil.move(str(target), str(previous/name))
            try:
                (staging/name).rename(target)
            except BaseException:
                if (previous/name).exists(): shutil.move(str(previous/name), str(target))
                raise
    return len(selected)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('seed', 'libraries', 'destination'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    print(f'MCD packaged {package(a.seed, a.libraries, a.destination)} offline Metal libraries; source fallback retained')


if __name__ == '__main__': main()
