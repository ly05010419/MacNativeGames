"""Read-only update planning and host-only updates using the installed App's inputs."""
import fcntl
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

from tmnt_bundle import check_output, install_app, record_update_source
from rewrite_modules import GAME, REPO, TOOL_SOURCES, sha256


def blocker(app):
    try:
        app = Path(app)
        check_output(app)
        resources = app / 'Contents/Resources/game'
        inputs = resources / 'runtime-update'
        report = json.loads((inputs / 'rewrite-report.json').read_text())
        if report['tools'] != {name: sha256(GAME / 'third_party/d2r' / name) for name in TOOL_SOURCES}:
            return 'TMNT rewriter changed; rebuild from the game file'
        from tmnt_shaders import chain, translation_sources
        seed = json.loads((resources / 'build/shader-seed/seed-manifest.json').read_text())
        if seed['chain'] != chain() or seed.get('translation_sources') != translation_sources() or seed.get('bindings') != sha256(GAME/'assets/shader_list.tsv'):
            return 'TMNT translation chain changed; rebuild from the game file'
        hashes = json.loads((inputs / 'inputs.json').read_text())
        required = {f'runtime-update/{name}' for name in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h', 'tmnt_nro_digest.h', 'rewrite-report.json')}
        if not required.issubset(hashes):
            return 'TMNT update headers are missing; rebuild once'
        for name, value in hashes.items():
            path = resources / name
            if Path(name).is_absolute() or '..' in Path(name).parts or not path.resolve().is_relative_to(resources.resolve()) or sha256(path) != value:
                return 'TMNT installed update inputs changed; rebuild from the game file'
        modules = {f'rewritten/{module}.{suffix}' for module in ('main','subsdk0','subsdk1','subsdk2') for suffix in ('patched.bin','fixups.tsv','gates.tsv')}
        if not modules.issubset(hashes) or not any(name.startswith('rewritten/nro/') for name in hashes):
            return 'TMNT rewrite identity missing; rebuild once'
    except (OSError, ValueError, KeyError, TypeError):
        return 'TMNT App predates runtime-update metadata or has incomplete inputs; rebuild once'
    return None


def update(app, work):
    app, work = Path(app).resolve(), Path(work).resolve()
    if work == REPO or work.is_relative_to(REPO) or REPO.is_relative_to(work) or work == app or work.is_relative_to(app) or app.is_relative_to(work):
        raise ValueError('update work must be outside the checkout and App')
    problem = blocker(app)
    if problem:
        raise ValueError(problem)
    work.mkdir(parents=True, exist_ok=True)
    from build_app import wait_until_closed
    with (work / 'tmnt-update.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        resources = app / 'Contents/Resources/game'
        generated = work / 'build'; generated.mkdir(exist_ok=True)
        for header in (resources / 'runtime-update').glob('*.h'):
            shutil.copy2(header, generated / header.name)
        subprocess.run(['make', '-C', str(GAME), '-j8', 'runtime', f'WORK={work}', f'PY={sys.executable}'], check=True)
        subprocess.run([str(generated / 'tmnt-native'), str(resources / 'rewritten'), 'link'], check=True,
                       env=dict(os.environ, TOTK_AUDIO_OFF='1', TOTK_CLOUD_SAVE_DIR='0'))
        # Recheck the retained translations and exact pipeline list with the new host.
        from tmnt_pipelines import mac_pipelines
        seed = generated / 'shader-seed'
        if seed.exists(): shutil.rmtree(seed)
        subprocess.run(['cp', '-cR', str(resources / 'build/shader-seed') + '/', str(seed)], check=True)
        mac_pipelines(generated / 'tmnt-native', resources / 'rewritten', generated)
        # APFS cloning preserves game bytes, translations and their identity.
        with tempfile.TemporaryDirectory(prefix='.tmnt-update-', dir=app.parent) as temp:
            partial = Path(temp) / app.name
            subprocess.run(['cp', '-cR', str(app), str(partial)], check=True)
            target = partial / 'Contents/Resources/game/build/tmnt-native'
            shutil.copy2(generated / 'tmnt-native', target)
            archive = partial / 'Contents/Resources/game/build/pipeline-archive'
            shutil.rmtree(archive)
            shutil.copytree(generated / 'pipeline-archive', archive)
            for name in ('pso_manifest.tsv', 'shader-map.tsv'):
                shutil.copy2(generated / name, partial / 'Contents/Resources/game/build' / name)
            # Do not retain the resolver's intermediate title list from older updates.
            (partial / 'Contents/Resources/game/build/pso_title.tsv').unlink(missing_ok=True)
            from package_private import write_app_entry
            info_path = partial / 'Contents/Info.plist'
            info = plistlib.loads(info_path.read_bytes())
            write_app_entry(partial, sha256(target))
            record_update_source(info, work); info_path.write_bytes(plistlib.dumps(info))
            manifest = partial / 'Contents/Resources/game/BUILD-MANIFEST.json'
            subprocess.run(['codesign', '--force', '--sign', '-', str(target)], check=True)  # nested: not the app
            data = json.loads(manifest.read_text()); data['runtime_sha256'] = sha256(target)
            manifest.write_text(json.dumps(data, indent=2) + '\n')
            subprocess.run(['codesign', '--force', '--sign', '-', f'-r=designated => identifier "{info["CFBundleIdentifier"]}"', str(partial)], check=True)
            wait_until_closed(app)
            install_app(partial, app)
    print(f'TMNT runtime updated: {app}')
