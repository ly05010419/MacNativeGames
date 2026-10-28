#!/usr/bin/env python3
"""MacNativeGames: put a new build of the runtime into an app that build_app.py made, without its game files.

    python3 update_runtime.py "<the app>" [--work <folder>] [--seed]

An app has two parts. The runtime is made from this repository alone: the game binary (games/totk/runtime and the graphics
layer), the launcher, the shader tools (SPIRV-Cross, the shader translator) and the warm-up lists exported from the shared
shader list. Everything else comes from the player's own game files: the rewritten code modules, the RomFS and its overlays,
the shared fonts, the startup shader seed and the game data folder the runtime reads at start (build/game-data/<version>:
module tables and hashes, games/totk/runtime/game_data.h). This builds the first part and puts it into the app in place of
the old one; the second part is not read again or changed, so it needs no game file and no keys, and takes a few minutes
instead of a full build. The game and its version are those of the app.

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


def replace_seed(tree, tmp, version, game):
    """The new seed and its libraries in the app being assembled (tmp), in place of the old ones (moved out of it and
    deleted in the background: 128,000 files), and the seed's aliases with the game data."""
    new_res, new_data = tmp / 'Contents/Resources/totk', tmp / f'Contents/Resources/totk/build/game-data/{version}'
    for rel in ('build/shader-seed', 'build/shader-libs'):
        old = new_res / rel
        if old.exists():
            aside = tmp.with_name(f'{tmp.name}.{old.name}.old')
            if aside.exists():
                build_app.remove_tree(aside)
            old.rename(aside)
            build_app.remove_in_background(aside)
    build_app.place_seed(tree, new_res, version, game=game)
    aliases = tree / f'build/shader-seed/{version}.aliases.tsv'
    (new_data / 'shader-seed.aliases.tsv').unlink(missing_ok=True)
    if aliases.is_file():
        clone(aliases, new_data / 'shader-seed.aliases.tsv')


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


def probe_runtime(tmp, version, logs):
    """The new runtime against the app's own game files, before anything is replaced: the loader probe (loader.c probe)
    reads the game data, maps the modules and checks their SHA-256, links them and calls across them, with no window, GPU
    or file written."""
    res, log = tmp / 'Contents/Resources/totk', logs / 'runtime-probe.log'
    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_'))}
    r = subprocess.run([res / 'build/totk-native', f'rewritten/{version}', 'probe'], cwd=res, env=env, capture_output=True,
                       text=True, timeout=600)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(r.stdout + r.stderr)
    if r.returncode or '"status":"pass"' not in r.stdout:
        last = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or ['no output'])[-1]
        raise BuildError(f'the new runtime does not load the app\'s game files ({last}; {log}); the app was not changed')


def record_commit(tmp):
    """The commits this runtime is built from, in the app's Info.plist: MNGSourceCommit (build_app.source_commit) for
    the HUD, which would otherwise keep showing the commit of the app's first build, and MNGSourceRevision
    (build_app.source_revision) for app/update_check.h, which would otherwise keep offering the update the app already
    has, because the update notice compares GitHub main against the app's first build only."""
    path = tmp / 'Contents/Info.plist'
    info = plistlib.loads(path.read_bytes())
    commit = build_app.source_commit()
    if commit:
        info['MNGSourceCommit'] = commit
    else:
        info.pop('MNGSourceCommit', None)
    revision = build_app.source_revision()
    if revision:
        info['MNGSourceRevision'] = revision
    else:
        info.pop('MNGSourceRevision', None)
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
    for bundle in sorted((failed / 'Contents/Helpers').glob('GameData-*.bundle')):
        if not (helpers / bundle.name).exists():
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


def seed_aliases(tree, res, version, new_data):
    """The seed's aliases the warm-up lists are renamed with (build_app.place_runtime): the app's copy, or the work folder's
    when its seed is the app's (an older app has no copy). False when not known: the app's lists are then kept."""
    if (res / f'build/game-data/{version}/shader-seed.aliases.tsv').is_file():
        return True
    seed = tree / f'build/shader-seed/{version}'
    if not ((seed / 'seed-manifest.json').is_file() and (res / 'build/shader-seed/seed-manifest.json').is_file()
            and sha256(seed / 'seed-manifest.json') == sha256(res / 'build/shader-seed/seed-manifest.json')):
        return False
    aliases = seed.with_name(seed.name + '.aliases.tsv')
    if aliases.is_file():
        clone(aliases, new_data / 'shader-seed.aliases.tsv')
    return True


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app', type=Path, help='the app build_app.py made (Tears of the Kingdom.app, Breath of the Wild.app)')
    ap.add_argument('--work', type=Path, help='work folder to build in (default: TOTK_build or BotW_build next to the app); an '
                                              'app made before the game data folder existed, and --seed, need the one of its build')
    ap.add_argument('--seed', action='store_true', help='also make the startup shaders again from the work folder\'s RomFS and '
                                                        'modules (no game file or keys), as needed when the translation chain changed')
    args = ap.parse_args(argv)
    app = args.app.expanduser().resolve()
    game, version = app_identity(app)
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
    stale_overlays = check_overlays(res, version)
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
    say('building the runtime from this repository (the game binary, the launcher, the shader tools)')
    run(['make', f'-j{os.cpu_count() or 4}', 'binary' if version == 'v0' else f'binary-{version}'], logs / 'runtime.log', cwd=tree, env=env)
    build_app.build_launcher(tree, version, logs, env)
    if not (tree / 'build/spirv-cross/spirv-cross').exists():
        build_app.build_spirv_cross(tree, logs)
    build_app.check_translator(tree)
    say(f'assembling the updated app next to {app.name}')
    tmp = app.with_name(app.name + '.partial')
    if tmp.exists():
        build_app.remove_tree(tmp)
    build_app.copy_tree(app, tmp)   # one APFS clone of the whole app
    try:
        new_res = tmp / 'Contents/Resources/totk'
        new_data = new_res / f'build/game-data/{version}'
        if data != res / f'build/game-data/{version}':
            for name in build_app.game_data_files(version):
                clone(data / name, new_data / name)
        if args.seed:
            replace_seed(tree, tmp, version, game)
        lists = True if args.seed else seed_aliases(tree, res, version, new_data)
        if not lists:
            say('   the startup shader seed of the app is not the work folder\'s, so the app keeps its warm-up lists')
        build_app.place_runtime(tree, tmp, version, logs, game=game, lists=lists)
        record_commit(tmp)
        probe_runtime(tmp, version, logs)
        build_app.sign_app(tmp, logs)
        build_app.wait_until_closed(app)
        replaced = []   # the replaced Contents are deleted only once the updated app passes its check
        if build_app.install_app(tmp, app, remove=replaced.append):
            say('   the game data is unchanged; kept the installed copy, so the first start need not verify it again')
    finally:
        if tmp.exists():
            build_app.remove_tree(tmp)
    run([build_app.LSREGISTER, '-f', app], logs / 'register-app.log')
    r = subprocess.run([app / f'Contents/MacOS/{APPS[game][2]}', '--check'], capture_output=True, text=True, timeout=1800)
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
        + ('its startup shaders were made again, its other game files are unchanged' if args.seed else 'its game files are unchanged'))


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as e:
        print(f'\nERROR: {e}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
