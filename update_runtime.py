#!/usr/bin/env python3
"""MacNativeGames: put a new build of the runtime into an app that build_app.py made, without its game files.

    python3 update_runtime.py "<the app>" [--work <folder>] [--seed]

An app has two parts. The runtime is made from this repository alone: the game binary (games/totk/runtime and the graphics
layer), the launcher, the shader tools (SPIRV-Cross, the shader translator) and the warm-up lists exported from the shared
shader list. Everything else comes from the player's own game files: the rewritten code modules, the RomFS and its overlays,
the shared fonts, the startup shader seed and the game data folder the runtime reads at start (build/game-data/<version>:
module tables and hashes, games/totk/runtime/game_data.h). This builds the first part and puts it into the app in place of
the old one. The other files are kept, except for optional 4K/5K canvas resources described below; it needs no new game
container or keys and takes a few minutes instead of a full build. The game and its version are those of the app.

Tears of the Kingdom 1.4.3 and Breath of the Wild 1.6.0 / 1.9.0 include a 5K choice by default on 32 GB-plus Macs when the work folder holds matching,
verified game files. An explicit --native-5k / --no-native-5k choice is saved in the app; it also honors one recorded by
build_app.py. With no choice, an existing 5K app keeps its resources on a smaller Mac. A missing cache or modded heap
overlays keep the installed canvas during an automatic update, with a rebuild message. No saved resolution is changed.

The startup shaders are game files too, but they must be of the runtime's shader translation chain: when a new runtime
changes the chain, the app is refused unless --seed is given, which makes them again from the work folder's RomFS and
modules (checked to be the app's) as build_app.py does, still without the game file or keys.

The code modules are game files as well, rewritten by the build's patch_nso.py, and a runtime can rely on what the
rewriter puts into them (gates, fixes moved from the runtime into the code). Modules that are the rewrites this
repository pins (expected-<version>.json) are accepted; others (a --mod build) only when the game data's record of their
rewriter (rewriter.txt, from build_src.sha256) is this repository's patch_nso.py. Otherwise the app needs build_app.py.

An app made before the game data folder existed (its game binary had the tables compiled in) gets the folder once, made
from the files the build that made the app extracted into its work folder (--work; default the TOTK_build or BotW_build
folder next to the app). Whatever the folder comes from, it is checked against the app's own modules, RomFS index and
fonts before anything is replaced.

The new app is assembled next to the installed one (an APFS clone, no extra space), its runtime is run against its game
files (the loader probe: the game data read, the modules mapped, checked, linked and called across, with no window, GPU or
writes), it is sealed and checked, and then takes the installed app's place as a build's does (build_app.install_app):
the installed game data is kept, so the first start does not verify the 16 GB again, and macOS keeps its first check of
the app folder. If the installed app then fails its --check, the replaced one is put back. A running game is waited for,
not closed.
"""
import argparse, json, os, plistlib, re, struct, subprocess, sys, time
from pathlib import Path

import build_app
from build_app import APPS, VERSIONS, BuildError, clone, run, say, sha256


def app_identity(app):
    """(game, version) of an app build_app.py made: the game by its executable, the version by its one set of modules."""
    if (app / 'Info.plist').is_file():
        raise BuildError(f'{app}: an iPhone / iPad app; this updates the Mac app only (build the iOS app again with build_app.py --platform ios)')
    try:
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    except (OSError, ValueError):
        raise BuildError(f'{app}: not an app (no readable Contents/Info.plist)') from None
    game = next((g for g, (_, _, executable) in APPS.items() if executable == info.get('CFBundleExecutable')), None)
    rewritten = app / 'Contents/Resources/totk/rewritten'
    versions = sorted(p.name for p in rewritten.iterdir() if p.is_dir()) if rewritten.is_dir() else []
    if not game or len(versions) != 1 or versions[0] not in VERSIONS or versions[0].startswith('botw') != (game == 'botw'):
        raise BuildError(f'{app}: not an app build_app.py made (expected Contents/MacOS/TOTK or BotW and one version '
                         f'in Contents/Resources/totk/rewritten, found {info.get("CFBundleExecutable")} and {versions})')
    return game, versions[0]


def runtime_chain():
    """The translation chain of this repository's runtime (DMSH_CHAIN)."""
    text = (build_app.GAME / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
    return re.search(r'#define DMSH_CHAIN "([^"]+)"', text).group(1)


def check_seed_chain(res, chain, *, remake=False):
    """The runtime warms up from the app's startup shader seed only when the seed is of its own translation chain
    (totk_shader_rt.m, nvn_mtl.m); with another, the first start would translate every shader as it is met. A new chain
    changes the seed's shaders, so the app needs a new seed: --seed makes it from the work folder (remake=True), or
    build_app.py does. Returns whether the seed is of another chain."""
    try:
        found = json.loads((res / 'build/shader-seed/seed-manifest.json').read_text())['chain']
    except (OSError, ValueError, KeyError, TypeError):
        if remake:
            return True
        raise BuildError(f'{res}: no readable startup shader seed manifest; pass --seed to make one from the work folder, '
                         f'or do a full rebuild with build_app.py') from None
    if found != chain and not remake:
        raise BuildError(f'the app\'s startup shaders are of translation chain {found}, the runtime of this repository uses '
                         f'{chain}: the shaders changed, so the app needs a new seed. Pass --seed to make it from the work '
                         f'folder of the app\'s build (a few minutes, no game file or keys), or do a full rebuild with '
                         f'build_app.py; the app was not changed')
    return found != chain


def check_work_files(tree, data, version):
    """The work folder's modules and RomFS are the app's (the game data folder names both), so a seed made from them is
    the app's seed. The RomFS by the SHA-256 its extraction verified (index.json) and its size: it is not hashed again."""
    _, modules = tables_modules(data / 'runtime-tables.bin')
    for name, digest in sorted(modules.items()):
        image = tree / f'rewritten/{version}/{name}.patched.bin'
        if not image.is_file() or sha256(image) != digest:
            raise BuildError(f'{image}: not the app\'s {name} module, so the work folder {tree.parent} cannot make its '
                             f'startup shaders (pass --work <the work folder of the app\'s build>)')
    romfs, want = tree / f'extracted/{version}/romfs', config(data / 'romfs-config.txt')
    try:
        index = json.loads((romfs / 'index.json').read_text())
        same = (index.get('status') == 'pass' and index.get('romfs_sha256') == want['ROMFS_IMAGE_SHA256']
                and (romfs / 'romfs.bin').stat().st_size == int(want['ROMFS_IMAGE_SIZE']))
    except (OSError, ValueError, KeyError):
        same = False
    if not same:
        raise BuildError(f'{romfs}: not the RomFS of the app (or not extracted there), so the work folder {tree.parent} '
                         f'cannot make its startup shaders (pass --work <the work folder of the app\'s build>)')


def make_seed(tree, work, game, version, logs, env):
    """A new startup shader seed from the work folder's RomFS and modules, as build_app.py makes it (steps 3/4), the
    libraries compiled ahead for it, and the tool step_warm_metal compiles them into Metal's cache with when it cannot."""
    say('making the startup shaders from the work folder\'s game files (a few minutes; no game file or keys)')
    if game == 'botw':
        build_app.step_shaders_botw(tree, work / 'botw', logs, version=version)
    else:
        build_app.step_shaders(tree, logs, version=version)
    build_app.step_shader_libs(tree, logs, version=version)
    build_app.build_warm_tool(tree, logs, env)


def replace_seed(tree, tmp, version, game, logs, bundle_id):
    """The new seed and its libraries in the app being assembled (tmp), as its shader data bundle, in place of the old
    ones (moved out of it and deleted in the background: 128,000 files)."""
    new_res = tmp / 'Contents/Resources/totk'
    for old in (new_res / 'build/shader-seed', new_res / 'build/shader-libs', tmp / f'Contents/Helpers/ShaderData-{version}.bundle'):
        if old.is_symlink():
            old.unlink()
        elif old.exists():
            aside = tmp.with_name(f'{tmp.name}.{old.name}.old')
            if aside.exists():
                build_app.remove_tree(aside)
            old.rename(aside)
            build_app.remove_in_background(aside)
    build_app.place_seed(tree, tmp, new_res, version, logs, game=game, bundle_id=bundle_id)


def migrate_shader_data(tmp, version, logs, work, bundle_id):
    """An app made before the shader data bundle (build_app.shader_bundle): its own seed and libraries become one, once
    (signed in the work folder, about a minute); later updates move it from app to app."""
    res = tmp / 'Contents/Resources/totk/build'
    bundle = work / f'build/shader-bundles/migrated/ShaderData-{version}.bundle'
    build_app.seal_shader_bundle(bundle, res / 'shader-seed', sorted((res / 'shader-libs').glob('*.metallib')), version, logs, bundle_id)
    (tmp / 'Contents/Helpers').mkdir(parents=True, exist_ok=True)
    bundle.rename(tmp / 'Contents/Helpers' / bundle.name)   # the seed's pipelines-* stay out: mac_pipelines makes build/pipelines
    build_app.link_shader_data(tmp, tmp / 'Contents/Resources/totk', bundle.name)


def config(path):
    """A game data "KEY VALUE" file (games/totk/runtime/game_data.c)."""
    return dict(line.split(' ', 1) for line in path.read_text().splitlines() if line and not line.startswith('#'))


def tables_modules(path):
    """(version, {module: SHA-256}) from runtime-tables.bin (the layout of games/totk/runtime/loader.c load_tables)."""
    data = path.read_bytes()
    magic, version, count = struct.unpack_from('<8s8sI', data)
    if magic != b'TOTKTAB1':
        raise BuildError(f'{path}: not a runtime tables file')
    modules = {}
    for i in range(count):
        name, digest = struct.unpack_from('<16s72s', data, 64 + 208 * i)
        modules[name.rstrip(b'\0').decode()] = digest.rstrip(b'\0').decode()
    return version.rstrip(b'\0').decode(), modules


def check_game_data(data, res, version):
    """The game data folder describes the app's own game files: the tables name the SHA-256 of the app's modules, the
    RomFS (and add-on content) configuration that of the app's indexes, the fonts list that of the app's fonts. The runtime
    checks each at start; checked here too, so that a folder of another build stops this before the app is touched."""
    found, modules = tables_modules(data / 'runtime-tables.bin')
    if found != version:
        raise BuildError(f'{data}: game data of version {found}, the app is {version}')
    for name, digest in sorted(modules.items()):
        image = res / f'rewritten/{version}/{name}.patched.bin'
        if not image.is_file() or sha256(image) != digest:
            raise BuildError(f'{data}: made from other code modules than the app\'s ({name}); a full rebuild with '
                             f'build_app.py is required, which rewrites the game\'s code modules')
    if sha256(res / f'extracted/{version}/romfs/runtime-index.bin') != config(data / 'romfs-config.txt')['ROMFS_INDEX_SHA256']:
        raise BuildError(f'{data}: made for another RomFS index than the app\'s; a full rebuild with build_app.py is required')
    fonts = config(data / 'shared-fonts.txt')
    for i in range(6):
        font = res / 'extracted/firmware/shared-fonts' / fonts[f'FONT{i}_FILE']
        if not font.is_file() or sha256(font) != fonts[f'FONT{i}_SHA256']:
            raise BuildError(f'{data}: made for other shared fonts than the app\'s ({font.name}); a full rebuild with '
                             f'build_app.py is required')
    if version.startswith('botw'):
        aoc = config(data / 'aoc-config.txt')
        for index in (1, 2):
            if int(aoc['AOC_PRESENT_MASK']) >> (index - 1) & 1 and \
                    sha256(res / f'extracted/{version}/aoc/{index}/runtime-index.bin') != aoc[f'AOC{index}_INDEX_SHA256']:
                raise BuildError(f'{data}: made for other add-on content than the app\'s (volume {index}); a full rebuild '
                                 f'with build_app.py is required')


def check_overlays(res, version):
    """The picture-size RomFS overlays (romfs-patches-out/<size>/overlay.json) whose recorded script_sha256 is not this
    repository's make_output_res_overlay.py: those update_runtime.py cannot renew, only a full rebuild can (#1182)."""
    script = build_app.REPO / 'games/totk/scripts/make_output_res_overlay.py'
    want = sha256(script) if script.is_file() else None
    stale = []
    for record in sorted((res / f'extracted/{version}/romfs-patches-out').glob('*/overlay.json')):
        try:
            found = json.loads(record.read_text()).get('script_sha256')
        except (OSError, ValueError):
            found = None
        if want is None or found != want:
            stale.append(record.parent.name)
    return stale


def rewriter_sha256():
    """The SHA-256 of this repository's rewriter, as patch_nso.py records itself in build_src.sha256."""
    return sha256(build_app.GAME / 'third_party/d2r/patch_nso.py')


def check_rewriter(data, res, version, want):
    """The app's code modules are what this repository's rewriter makes. They are when they are the rewrites this
    repository pins (expected-<version>.json rewritten_sha256), whatever build_src.sha256 says: build_app.py does not run
    the rewriter again for modules that already match those, so its record can name an older patch_nso.py whose output for
    this version is byte for byte the same (a038e6b changed patch_nso.py for Breath of the Wild 1.9.0 only). Modules that
    are not the pinned ones (a --mod build) are judged by the rewriter that made them (rewriter.txt in the game data,
    written by prepare_runtime.py from build_src.sha256): another one may have put other gates into the code, or left out
    fixes that moved from the runtime into the rewriter (Breath of the Wild 1.9.0's job-queue spin wait, a038e6b /
    470aa5f), and this runtime would not know. Such an app needs build_app.py, which rewrites the modules."""
    pinned = VERSIONS[version].get('rewritten_sha256') or {}
    if pinned and all(sha256(res / f'rewritten/{version}/{name}') == digest for name, digest in pinned.items()):
        return
    try:
        found = config(data / 'rewriter.txt')['REWRITER_SHA256']
    except (OSError, ValueError, KeyError):
        raise BuildError(f'{data}: the app\'s code modules are not the rewrites this repository pins for '
                         f'{VERSIONS[version]["display_version"]}, and there is no record of the rewriter that made them '
                         f'(rewriter.txt); a full rebuild with build_app.py is required') from None
    if found != want:
        raise BuildError(f'the app\'s code modules are not the rewrites this repository pins for '
                         f'{VERSIONS[version]["display_version"]} (a build with a mod\'s code patches?), and another '
                         f'patch_nso.py ({found[:12]}…) than this repository\'s ({want[:12]}…) made them: this runtime may '
                         f'rely on gates or fixes those modules lack. A full rebuild with build_app.py is required, which '
                         f'rewrites the modules; the app was not changed')


def probe_runtime(tree, tmp, version, logs):
    """The new runtime against the app's own game files, before anything is replaced: the loader probe (loader.c probe)
    reads the game data, maps the modules and checks their SHA-256, links them and calls across them, with no window, GPU
    or file written. Run from outside the .partial bundle (build_app.run_outside_bundle): started inside it, macOS first
    checked the whole new bundle, 237 of the probe's 238 s and half of every update (2026-11-03)."""
    res, log = tmp / 'Contents/Resources/totk', logs / 'runtime-probe.log'
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_'))}
    with build_app.run_outside_bundle(res / 'build/totk-native', res / 'build/runtime-probe', tree / 'build/outside-bundle') as program:
        r = subprocess.run([program, f'rewritten/{version}', 'probe'], cwd=res, env=env, capture_output=True,
                           text=True, timeout=600)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(r.stdout + r.stderr)
    if r.returncode or '"status":"pass"' not in r.stdout:
        last = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or ['no output'])[-1]
        raise BuildError(f'the new runtime does not load the app\'s game files ({last}; {log}); the app was not changed')


def record_commit(tmp, work, *, native_5k=None):
    """The commit this runtime is built from, in the app's Info.plist: MNGSourceCommit (build_app.source_commit) for
    the HUD, which would otherwise keep showing the commit of the app's first build; and where the Update page's update
    finds the checkout, the work folder and the Python (build_app.record_update_source), so an app made before those were
    recorded updates itself from then on."""
    path = tmp / 'Contents/Info.plist'
    info = plistlib.loads(path.read_bytes())
    commit = build_app.source_commit()
    if commit:
        info['MNGSourceCommit'] = commit
    else:
        info.pop('MNGSourceCommit', None)
    build_app.record_update_source(info, work)
    if native_5k is None:
        info.pop('MNGNative5K', None)
    else:
        info['MNGNative5K'] = native_5k
    path.write_bytes(plistlib.dumps(info))


def restore_replaced(app, replaced):
    """Put the replaced app back after the updated one failed its check: the replaced Contents return to the same folder,
    with the game data bundle the update kept (keep_game_data moved it into the new Contents). Returns the failed app's
    folder, outside the app."""
    failed = app.with_name(f'{app.name}.failed-{os.getpid()}')
    if failed.exists():
        build_app.remove_tree(failed)
    failed.mkdir()
    for entry in list(app.iterdir()):
        entry.rename(failed / entry.name)
    for entry in list(replaced.iterdir()):
        entry.rename(app / entry.name)
    replaced.rmdir()
    helpers = app / 'Contents/Helpers'
    seal = app / 'Contents/_CodeSignature/CodeResources'
    sealed = seal.read_bytes() if seal.is_file() else b''
    for bundle in sorted([*(failed / 'Contents/Helpers').glob('GameData-*.bundle'), *(failed / 'Contents/Helpers').glob('ShaderData-*.bundle')]):
        # the bundles the restored app's seal names (an app made before the shader data bundle names none of those)
        if not (helpers / bundle.name).exists() and f'Helpers/{bundle.name}'.encode() in sealed:
            helpers.mkdir(parents=True, exist_ok=True)
            bundle.rename(helpers / bundle.name)
    return failed


def game_data(tree, res, version, logs, env):
    """The app's game data folder, or else one made once from the work folder (an app made before the folder existed)."""
    installed = res / f'build/game-data/{version}'
    if all((installed / name).is_file() for name in build_app.game_data_files(version)):
        return installed
    say('the app has no game data folder yet (it was made before the game binary read one): making it from the work folder')
    try:
        run(['make', f'game-data-{version}'], logs / 'game-data.log', cwd=tree, env=env)
    except BuildError as e:
        raise BuildError(f'the work folder {tree.parent} does not hold the files the build of this app extracted, so the game '
                         f'data folder cannot be made once (pass --work <the work folder of that build>, or do a full '
                         f'rebuild with build_app.py); make said: {str(e).splitlines()[0]}') from None
    return tree / f'build/game-data/{version}'


def prepare_vr_render_overlay(tree, res, data, version, logs, env):
    """Build the exact TotK eye-capacity heap from matching verified RomFS inputs."""
    if version != 'v143':
        raise BuildError('--vr updates currently require Tears of the Kingdom 1.4.3')
    romfs = tree / 'extracted/v143/romfs'
    try:
        index = json.loads((romfs / 'index.json').read_text())
        config_data = config(data / 'romfs-config.txt')
        valid = (index.get('status') == 'pass' and index.get('romfs_sha256') == config_data['ROMFS_IMAGE_SHA256']
                 and (romfs / 'romfs.bin').stat().st_size == int(config_data['ROMFS_IMAGE_SIZE']))
    except (OSError, ValueError, KeyError):
        valid = False
    if not valid:
        raise BuildError('--vr needs the matching verified RomFS index and image in the work folder; use the original build workspace')
    run([env.get('PY', sys.executable), 'scripts/make_output_res_overlay.py', '4096x4416',
         '--scene', '4096x4416', '--version', version], logs / 'vr-render-overlay.log', cwd=tree, env=env)
    return tree / 'extracted/v143/romfs-patches-out/4096x4416-scene-4096x4416'


def prepare_native_5k(tree, res, data, version, logs, preference):
    """Prepare a canvas upgrade from the matching cached game files, before touching the installed app.
    Existing 5K apps keep their capability on smaller Macs. A recorded or requested False takes priority."""
    if version in ('botw160', 'botw190'):
        folder = res / f'extracted/{version}'
        present = (folder / 'native-5k').is_file()
        enabled = preference if preference is not None else present or build_app.native_5k_default()
        if not enabled:
            return ('remove' if preference is False else 'keep'), None
        if present and (folder / 'native-4k').is_file():
            return 'keep', None
        # BotW's verified code owns these heaps; no RomFS heap overlays to generate.
        made = logs / 'native-5k-markers'
        made.mkdir(parents=True, exist_ok=True)
        (made / 'native-4k').write_text('3840x2160\n')
        (made / 'native-5k').write_text('5120x2880\n')
        say('   native 5K: enabling the code-owned 4K/5K canvases and heaps; saved resolution is kept')
        return 'add', made
    if version != 'v143':
        return 'keep', None
    overlays = res / 'extracted/v143/romfs-patches-out'
    present = (overlays / '5120x2880-scene-5120x2880/manifest.tsv').is_file()
    enabled = preference if preference is not None else present or build_app.native_5k_default()
    if not enabled:
        return ('remove' if preference is False else 'keep'), None
    if present:
        return 'keep', None
    try:
        check_work_files(tree, data, version)   # modules, RomFS size and the verified index must match this app
        for record in overlays.glob('*/overlay.json'):
            if json.loads(record.read_text()).get('heapdef'):
                raise BuildError('the app has modded heap overlays; rebuild with the same --mod and --native-5k')
    except (BuildError, OSError, ValueError, KeyError) as error:
        if preference is True:
            raise BuildError(f'cannot add native 5K from this work folder: {error}; rebuild with build_app.py --native-5k') from error
        say(f'   native 5K was not added: {error}; kept the installed canvas. Rebuild with build_app.py to add it')
        return 'keep', None
    made = logs / 'native-5k-overlays'
    if made.exists():
        build_app.remove_tree(made)
    for i, args in enumerate(build_app.large_output_overlay_args(native_5k=True)):
        run([tree / '.venv/bin/python', 'scripts/make_output_res_overlay.py', *args,
             '--version', version, '--output-root', made], logs / f'native-5k-overlay-{i}.log', cwd=tree)
    say('   native 5K: adding the 4K/5K canvas resources from the verified build cache; saved resolution is kept')
    return 'add', made


def replace_native_5k(res, action, made, version='v143'):
    """Only the staged app changes; explicit disabling drops 5K while retaining its 4K resources."""
    if version in ('botw160', 'botw190'):
        folder = res / f'extracted/{version}'
        if action == 'remove':
            (folder / 'native-5k').unlink(missing_ok=True)
        elif action == 'add':
            for name in ('native-4k', 'native-5k'):
                clone(made / name, folder / name)
        return
    overlays = res / 'extracted/v143/romfs-patches-out'
    if action == 'remove':
        for folder in overlays.glob('5120x2880*'):
            build_app.remove_tree(folder)
    elif action == 'add':
        for folder in made.iterdir():
            target = overlays / folder.name
            if target.exists():
                build_app.remove_tree(target)
            build_app.copy_tree(folder, target)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app', type=Path, help='the app build_app.py made (Tears of the Kingdom.app, Breath of the Wild.app)')
    ap.add_argument('--work', type=Path, help='work folder to build in (default: TOTK_build or BotW_build next to the app); an '
                                              'app made before the game data folder existed, and --seed, need the one of its build')
    ap.add_argument('--vr', action='store_true', help='prepare TotK 1.4.3 exact VR render targets from its verified build inputs')
    ap.add_argument('--seed', action='store_true', help='also make the startup shaders again from the work folder\'s RomFS and '
                                                        'modules (no game file or keys), as needed when the translation chain changed')
    ap.add_argument('--native-5k', action=argparse.BooleanOptionalAction, default=None,
                    help='Mac TOTK 1.4.3 / BotW 1.6.0 and 1.9.0: include/omit 5K; without a recorded choice, include it on 32 GB-plus Macs '
                         'when the matching verified build cache is available. Saved resolution is not changed')
    args = ap.parse_args(argv)
    app = args.app.expanduser().resolve()
    game, version = app_identity(app)
    if args.native_5k and version not in ('v143', 'botw160', 'botw190'):
        raise BuildError('--native-5k needs Tears of the Kingdom 1.4.3 or Breath of the Wild 1.6.0 / 1.9.0 on Mac')
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    preference = args.native_5k if args.native_5k is not None else info.get('MNGNative5K')
    if preference is not None and type(preference) is not bool:
        raise BuildError('MNGNative5K must be a boolean (or absent for automatic selection)')
    stale = check_seed_chain(app / 'Contents/Resources/totk', runtime_chain(), remake=args.seed)
    work = args.work.expanduser().resolve() if args.work else app.parent / ('BotW_build' if game == 'botw' else 'TOTK_build')
    py = build_app.prerequisites(args)
    logs = work / 'logs'
    env = dict(os.environ, PATH=os.pathsep.join(build_app.TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))
    if game == 'botw':   # the game's own shared shader list (build_app.py main)
        env['SHADER_LIST'] = '../botw/assets/shader_list.tsv'
    t0 = time.time()
    say(f'{APPS[game][0]} {VERSIONS[version]["display_version"]}: {app}; work folder {work}')
    tree = build_app.prepare_work(work, py, logs)
    res = app / 'Contents/Resources/totk'
    data = game_data(tree, res, version, logs, env)
    check_game_data(data, res, version)
    check_rewriter(data, res, version, rewriter_sha256())
    canvas_action, canvas_source = prepare_native_5k(tree, res, data, version, logs, preference)
    vr_overlay = prepare_vr_render_overlay(tree, res, data, version, logs, env) if args.vr else None
    stale_overlays = check_overlays(res, version)
    if canvas_action == 'add':   # this promotion also renews the stock 4K overlays; smaller overlays stay as they were
        stale_overlays = [name for name in stale_overlays if not name.startswith(('3840x2160', '5120x2880'))]
    if stale_overlays:
        say(f'   the app\'s picture-size RomFS overlays ({", ".join(stale_overlays)}) were made by another '
            f'make_output_res_overlay.py than this repository\'s: the runtime is updated, but only a full rebuild with '
            f'build_app.py renews those overlays (after PR #1222)')
    else:
        say('   the app\'s code modules are this repository\'s rewrites, its game data matches its game files and its '
            'picture-size overlays its make_output_res_overlay.py: updating the runtime is enough for those. The other '
            'RomFS patches the build made (romfs-patches) keep no record to check here: after changes to them, rebuild')
    if args.seed:
        check_work_files(tree, data, version)
        if not stale:
            say('   the app\'s startup shaders are of this runtime\'s translation chain; making them again as asked (--seed)')
        make_seed(tree, work, game, version, logs, env)
    say('building the runtime from this repository (the game binary with its launcher part, the shader tools)')
    run(['make', f'-j{os.cpu_count() or 4}', 'binary' if version == 'v0' else f'binary-{version}'], logs / 'runtime.log', cwd=tree, env=env)
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        build_app.build_spirv_cross(tree, logs)
    build_app.check_translator(tree)
    say(f'assembling the updated app next to {app.name}')
    tmp = app.with_name(app.name + '.partial')
    if tmp.exists():
        build_app.remove_tree(tmp)
    bundle_id = plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier') or APPS[game][1]
    shaders = None if args.seed else build_app.app_shader_bundle(app, version)
    if shaders:   # its shader data stays where it is: read through a link now, moved into the new app at the end
        build_app.clone_app_except(app, tmp, shaders)
    else:
        build_app.copy_tree(app, tmp)   # one APFS clone of the whole app
    try:
        new_res = tmp / 'Contents/Resources/totk'
        new_data = new_res / f'build/game-data/{version}'
        if data != res / f'build/game-data/{version}':
            for name in build_app.game_data_files(version):
                clone(data / name, new_data / name)
        if args.seed:
            replace_seed(tree, tmp, version, game, logs, bundle_id)
        elif not shaders:
            migrate_shader_data(tmp, version, logs, work, bundle_id)
        replace_native_5k(new_res, canvas_action, canvas_source, version)
        if vr_overlay:
            destination = new_res / f'extracted/{version}/romfs-patches-out' / vr_overlay.name
            if destination.exists():
                build_app.remove_tree(destination)
            build_app.copy_tree(vr_overlay, destination)
        lists = (new_res / 'build/shader-seed/shader-keys.json').is_file()   # the seed resolve names the rows by
        if not lists:
            say('   the app\'s startup shader seed predates its shader-keys.json, so the app keeps its warm-up lists (--seed makes a new one)')
        build_app.place_runtime(tree, tmp, version, logs, game=game, lists=lists)
        record_commit(tmp, work, native_5k=preference)
        probe_runtime(tree, tmp, version, logs)
        build_app.mac_pipelines(tree, new_res, version, logs)   # the new binary's pipeline keys, as a build makes them
        if shaders:
            build_app.shader_signature_only(tmp, shaders)
        t = time.time(); build_app.sign_app(tmp, logs); say(f'   signed ({time.time() - t:.0f} s)')
        build_app.wait_until_closed(app)
        t = time.time()
        replaced = []   # the replaced Contents are deleted only once the updated app passes its check
        if build_app.install_app(tmp, app, remove=replaced.append):
            say('   the game data is unchanged; kept the installed copy, so the first start need not verify it again')
    finally:
        if tmp.exists():
            build_app.remove_tree(tmp)
    say(f'   installed, its seal checked ({time.time() - t:.0f} s)')
    t = time.time(); run([build_app.LSREGISTER, '-f', app], logs / 'register-app.log'); say(f'   registered with macOS ({time.time() - t:.0f} s)')
    t = time.time(); r = subprocess.run([app / f'Contents/MacOS/{APPS[game][2]}', '--check'], capture_output=True, text=True, timeout=1800)
    say(f'   the app\'s own check ({time.time() - t:.0f} s)')
    mine = app.with_name(f'{app.name}.replaced-{os.getpid()}')
    if r.returncode and mine in replaced and mine.is_dir():
        build_app.remove_in_background(restore_replaced(app, mine))
        run([build_app.LSREGISTER, '-f', app], logs / 'register-app.log')
        for path in replaced:
            if path != mine:
                build_app.remove_in_background(path)
        raise BuildError(f'the updated app does not start ({(r.stdout + r.stderr).strip()}); the previous app was put back')
    for path in replaced:
        build_app.remove_in_background(path)
    if r.returncode:
        raise BuildError(f'the app does not start: {(r.stdout + r.stderr).strip()}')
    if args.seed:
        build_app.step_warm_metal(tree, app, logs)
    say(f'done in {time.time() - t0:.0f} s: {app} has the runtime of this repository; '
        + ('its 4K/5K canvas resources were updated; ' if canvas_action != 'keep' else '')
        + ('its startup shaders were made again, its other game files are unchanged' if args.seed else 'its other game files are unchanged'))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
