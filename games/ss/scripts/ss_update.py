"""Express update planning and execution for the isolated SS runtime."""
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

from build_ss import GAME, copy_tree, stage, environment
from package_ss import TITLE, PACKAGE, package, verify_rewrite
from ss_bundle import adopt_inputs
from inspect_inputs import sha256


def is_app(app):
    try:
        return plistlib.loads((app/'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier') == 'org.macnativegames.ss'
    except (OSError, ValueError):
        return False


def load_chain():
    path = GAME/'runtime/gfx/tools/dmsh_chain.py'
    spec = importlib.util.spec_from_file_location('ss_chain', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def blocker(app):
    """Read-only: bind the App's modules and libraries to the current sources."""
    try:
        resources = app/'Contents/Resources/game'
        report = json.loads((resources/'build-info.json').read_text())
        chain = load_chain()
        if report['chain'] != chain.version():
            return 'the shader translation inputs changed; a full build is required'
        pins = json.loads((GAME/'assets/expected-1.0.1.json').read_text())['observed']
        if report.get('game') != pins:
            return 'the app was built from different game files; a full build is required'
        libraries = resources/'shader-cache'/f'b-{report["chain"]}'
        stamp = dict(line.split('\t', 1) for line in (libraries/'inputs.tsv').read_text().splitlines()
                     if '\t' in line and not line.startswith('#'))
        if stamp != {'game': TITLE, 'inputs': report['shader_inputs']}:
            return 'the installed shader library stamp differs; a full build is required'
        if not (app/'Contents/Helpers/GameData-ss.bundle/Contents/Resources/ExeFS').is_dir():
            return 'this older app has no verified original modules; rebuild once'
        patched = resources/'patched'
        # Rewrite rules are relative to dm/tools; check against runtime/tools here.
        for line in (patched/'build_src.sha256').read_text().splitlines():
            digest, rel = line.split('  ', 1)
            path = (GAME/'runtime/tools'/rel).resolve()
            if not path.is_relative_to(GAME/'runtime/tools') or sha256(path) != digest:
                return 'the code rewriter changed; a full build is required'
        proof = json.loads((patched/'rewrite-info.json').read_text())
        for name, digest in proof['outputs'].items():
            if Path(name).name != name or sha256(patched/name) != digest:
                return 'a rewritten module changed; a full build is required'
        profile = resources/'build'
        class Profile:
            rows = [line.split('\t', 1) for line in (GAME/f'runtime/games/{TITLE}/game.tsv').read_text().splitlines()
                    if line and not line.startswith('#') and '\t' in line]
            rows = [(k, v.split('\t')) for k, v in rows]
            def table(self, name):
                current = GAME/f'runtime/games/{TITLE}'/name
                return current if current.is_file() else profile/name
        if report['shader_inputs'] != chain.inputs(Profile()):
            return 'the shader profile inputs changed; a full build is required'
    except (OSError, ValueError, KeyError, TypeError):
        return 'the app has incomplete build metadata; a full build is required'
    return None


def build_host(source, build, env):
    spec = importlib.util.spec_from_file_location('ss_host_builder', source/'scripts/build_ipa_from_nsp.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Load the profile from the staged tree, independently of another game's imports.
    spec = importlib.util.spec_from_file_location('ss_game', source/'dm/games/game.py')
    game = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(game)
    (build/'tools').mkdir(parents=True, exist_ok=True)
    (build/'logs').mkdir(parents=True, exist_ok=True)
    return module.mac_host(sys.executable, source, build, env, build/'logs', game.load(TITLE))


def update(app, work):
    import fcntl
    app, work = Path(app).resolve(), Path(work).resolve()
    checkout = GAME.parents[1]
    if work == checkout or checkout in work.parents or work in checkout.parents or work == app or app in work.parents:
        raise ValueError('the update work folder must be outside the checkout and App')
    problem = blocker(app)
    if problem:
        raise ValueError(problem)
    from ss_python import build_python
    python = build_python(work)
    if Path(sys.prefix).resolve() != (work/'venv').resolve():
        subprocess.run([str(python), str(GAME.parents[1]/'update_runtime.py'), str(app), '--work', str(work)], check=True)
        return
    work.mkdir(parents=True, exist_ok=True)
    with (work/'build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        resources = app/'Contents/Resources/game'
        build = work/'build'
        extract = build/f'extract/{PACKAGE}'
        if not extract.exists():
            if not adopt_inputs(app, extract):
                raise ValueError('installed app has no reusable inputs')
        # Always take the installed app's data, not a stale work folder's output.
        patched = build/f'patched_{PACKAGE}'
        if patched.exists():
            shutil.rmtree(patched)
        subprocess.run(['cp', '-cR', str(resources/'patched')+'/', str(patched)], check=True)
        copy_tree(resources/'build', work/f'source/dm/games/{TITLE}')
        env = environment(work)
        source = stage(work, env, translator=False)
        report = json.loads((resources/'build-info.json').read_text())
        libraries = build/'shader/cache'/f'b-{report["chain"]}'
        if not libraries.exists():
            libraries.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['cp', '-cR', str(resources/'shader-cache'/libraries.name), str(libraries)], check=True)
        archive = build/'pipeline-archive'
        if not archive.exists() and (resources/'pipeline-archive').is_dir():
            subprocess.run(['cp', '-cR', str(resources/'pipeline-archive'), str(archive)], check=True)
        build_host(source, build, env)
        package(source, build, app, (resources/'language.txt').read_text().strip(), env)
