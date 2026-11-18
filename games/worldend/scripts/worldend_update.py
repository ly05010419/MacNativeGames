"""Rebuild the host while retaining checked translations, game data and pipeline entries."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from build_worldend import GAME, TITLE, stage, environment, package, translation_files, verify_originals
from worldend_bundle import Digests, check_output, verified_signature


def blocker(app):
    app = Path(app)
    try:
        check_output(app)
        resources = app/'Contents/Resources/game'
        report = json.loads((resources/'build-info.json').read_text())
        if report.get('game') != json.loads((GAME/'assets/expected-1.0.0.json').read_text()):
            return 'the game identity changed; a full build is required'
        if report.get('translation_files') != translation_files():
            return 'shader translation inputs changed; a full build is required'
        for line in report['rewrite_rules'].splitlines():
            digest, name = line.split('  ', 1)
            path = (GAME/'runtime/tools'/name).resolve()
            if not path.is_relative_to(GAME/'runtime') or not path.is_file() or __import__('hashlib').sha256(path.read_bytes()).hexdigest() != digest:
                return 'rewrite rules changed; a full build is required'
        if not (app/'Contents/Helpers/GameData-worldend.bundle/Contents/Resources/original/ExeFS').is_dir():
            return 'this older app has no verified data bundle; rebuild once'
    except (OSError, ValueError, KeyError, TypeError):
        return 'the app has incomplete build metadata; a full build is required'
    return None


def update(app, work):
    app, work = Path(app).resolve(), Path(work).resolve()
    repo = GAME.parents[1]
    if work == repo or repo in work.parents or work in repo.parents or work == app or app in work.parents:
        raise ValueError('work must stay outside the checkout and App')
    problem = blocker(app)
    if problem: raise ValueError(problem)
    from worldend_python import build_python
    python = build_python(work)
    if Path(sys.prefix).resolve() != (work/'venv').resolve():
        subprocess.run([str(python), str(__file__), str(app), str(work)], check=True)
        return
    import fcntl
    work.mkdir(parents=True, exist_ok=True)
    with (work/'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        env = environment(work)
        source = stage(work)
        build = work/'input'/f'{TITLE}_build'
        build.mkdir(parents=True, exist_ok=True)
        digests = Digests(build/'verified-digests.json')
        resources = app/'Contents/Resources/game'
        for name in ('GameData', 'ShaderData'):
            bundle = app/f'Contents/Helpers/{name}-worldend.bundle'
            manifest = json.loads((bundle/'Contents/Resources/files.json').read_text())
            if any(Path(rel).is_absolute() or '..' in Path(rel).parts for rel in manifest):
                raise ValueError('invalid data manifest path')
            if not verified_signature(bundle, digests, env) or any(digests.sha256(bundle/'Contents/Resources'/rel) != sha for rel, sha in manifest.items()):
                raise ValueError(f'{name} bundle changed; rebuild from the original inputs')
        data = app/'Contents/Helpers/GameData-worldend.bundle/Contents/Resources'
        def copy(src, dst):
            if dst.exists(): return
            dst.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['cp', '-cR', str(src), str(dst)], check=True)
        copy(data/'original', build/'extract/v0')
        for name, rel in (('patched', 'patched_v0'), ('font', 'font')): copy(data/name, build/rel)
        verify_originals(build)
        report = json.loads((resources/'build-info.json').read_text())
        if digests.files(build/'patched_v0') != report['patched']:
            raise ValueError('cached rewritten modules differ from the verified app')
        copy(resources/'shader-cache'/f'b-{report["chain"]}', build/'shader/cache'/f'b-{report["chain"]}')
        shader_files = json.loads((app/'Contents/Helpers/ShaderData-worldend.bundle/Contents/Resources/files.json').read_text())
        if any(digests.sha256(build/'shader/cache'/rel) != sha for rel, sha in shader_files.items()):
            raise ValueError('cached shader libraries differ from the verified app; rebuild the shaders')
        copy(resources/'pipeline-archive', build/'pipeline-archive')
        for name in ('c1_full.tsv', 'cb_extent.tsv'):
            __import__('shutil').copy2(resources/'build'/name, source/f'dm/games/{TITLE}'/name)
        spec = importlib.util.spec_from_file_location('worldend_host_builder', source/'scripts/build_ipa_from_nsp.py')
        builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
        spec = importlib.util.spec_from_file_location('worldend_profile', source/'dm/games/game.py')
        game = importlib.util.module_from_spec(spec); spec.loader.exec_module(game)
        (build/'logs').mkdir(exist_ok=True)
        builder.build_mac_host(source, build/'tools/dmhost', env, build/'logs', game.load(TITLE))
        package(source, build, app, report.get('language', 'en-US'), env)
        digests.save()


if __name__ == '__main__':
    update(Path(sys.argv[1]), Path(sys.argv[2]))
