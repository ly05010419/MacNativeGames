#!/usr/bin/env python3
"""MacNativeGames: bring an app's shaders and pipelines up to the newest shared shader list, without a runtime update.

    python3 shader_update.py "<the app>" [--check] [--list <shader_list.tsv>] [--work <folder>] [--json]

Players' games send the shaders and pipelines they meet to the shader inbox; they are merged into each game's shared list
(games/<game>/assets/shader_list.tsv) on GitHub. An app carries what its last build or update took from the list: the
translated variants in its startup shader seed, the warm-up lists (build/pso_title.tsv, pso_manifest.tsv) and the
pipeline archive made from them (build/pipelines). This downloads the game's newest list from GitHub (the one file, never
the code; --list uses a file instead) and compares it with the app (--check: nothing is changed):

  new shaders     variants the list names for this game version that no variant of the app's seed serves (the runtime's
                  own pick rule, prepare_shader_seed.deps_match); they need translating
  new pipelines   warm-up rows the app's seed serves already, not in its lists yet: only their pipelines are made
  waiting         warm-up rows that name one of the new shaders: made once those are translated

Without --check the app is updated with what is new: the new shaders translated from the build's work folder (the
shader code the build extracted; no game file or keys), their libraries compiled, the warm-up lists resolved from the
downloaded list, the new pipelines made into the archive by the app's own game binary, and the app sealed and put in
place as update_runtime.py does. The runtime is not rebuilt. Without the work folder only the new pipelines are added
and the report says how many shaders wait for a build that has one.

Tears of the Kingdom and Breath of the Wild (build_app.py apps) and Ocarina of Time (games/oot/build.py update_shaders,
its own list tools in games/oot/scripts).
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parent
SCRIPTS = REPO / 'games/totk/scripts'
RAW = 'https://raw.githubusercontent.com/ly05010419/MacNativeGames/main'
LISTS = {'totk': 'games/totk/assets/shader_list.tsv', 'botw': 'games/botw/assets/shader_list.tsv',
         'oot': 'games/oot/assets/shader_list.tsv'}
TOOLS = {'oot': REPO / 'games/oot/scripts'}   # a game with its own list tools; the others use games/totk/scripts


def scripts(game='totk'):
    """The game's list tools (shader_list, prepare_shader_seed): Ocarina of Time's own, else games/totk/scripts. Both
    folders name their modules alike, so another game's are dropped from the module cache first."""
    folder = str(TOOLS.get(game, SCRIPTS))
    import importlib
    for name in ('shader_list', 'prepare_shader_seed'):
        loaded = sys.modules.get(name)
        if loaded and Path(loaded.__file__).parent != Path(folder):
            del sys.modules[name]
    if folder in sys.path:
        sys.path.remove(folder)
    sys.path.insert(0, folder)
    return importlib.import_module('shader_list'), importlib.import_module('prepare_shader_seed')


def download(game, folder):
    """The game's newest shared list from GitHub into folder (/usr/bin/curl: python.org's Python has no CA certificates
    until its Install Certificates step). Returns the file; raises RuntimeError with what went wrong."""
    if game not in LISTS:
        raise RuntimeError(f'no shared shader list for {game}')
    out = Path(folder) / f'{game}-shader_list.tsv'
    r = subprocess.run(['/usr/bin/curl', '--fail', '--silent', '--show-error', '--location', '--compressed',
                        '--max-time', '300', '-o', str(out), f'{RAW}/{LISTS[game]}'], capture_output=True, text=True)
    if r.returncode or not out.is_file() or not out.stat().st_size:
        raise RuntimeError(f'could not download the shader list from GitHub: {(r.stderr or "empty file").strip()}')
    return out


def identity(app):
    """(game, version): Ocarina of Time's app ('oot', 'oot'), else a build_app.py app's (update_runtime.app_identity)."""
    _, ur = tools()
    if ur.ocarina_app(Path(app)):
        return 'oot', 'oot'
    return ur.app_identity(Path(app))


def rows_of(path):
    """The warm-up rows of a list file the app carries (tab-separated, '#' lines are comments)."""
    if not Path(path).is_file():
        return set()
    return {tuple(line.split('\t')) for line in Path(path).read_text().splitlines() if line and not line.startswith('#')}


def seed_variants(seed, key, cache):
    """The variants the app's seed carries for a shader (its <key>.variants rows with their deps)."""
    if key not in cache:
        try:
            rows = json.loads((Path(seed) / f'{key}.variants').read_text())
        except (OSError, ValueError):
            rows = []
        cache[key] = [r for r in rows if isinstance(r, dict) and isinstance(r.get('variant'), str)]
    return cache[key]


def new_shaders(lst, seed, pss):
    """The list's (key, deps) variants of this seed's game version that no variant of the seed serves."""
    keys = set(json.loads((Path(seed) / 'shader-keys.json').read_text())['keys'])
    cache, out = {}, []
    for key, deps in sorted(lst.variants):
        if key not in keys:
            continue   # another version's shader
        answers = pss.answers_for(json.loads(deps))
        if not any(pss.deps_match(v.get('deps') or {}, answers) for v in seed_variants(seed, key, cache)):
            out.append((key, deps))
    return out


def check(app, listfile):
    """What the list adds to the app, changing nothing: a dict with the counts and the reasons."""
    game, version = identity(app)
    sl, pss = scripts(game)
    res = Path(app) / 'Contents/Resources/totk'
    seed = res / 'build/shader-seed'
    if not (seed / 'shader-keys.json').is_file():
        raise RuntimeError('the app\'s startup shaders predate shader-keys.json: update the app first (update_runtime.py --seed)')
    lst = sl.ShaderList.load(listfile)
    shaders = new_shaders(lst, seed, pss)
    title, counts_t = lst.resolve(seed, 't')
    rest, counts_m = lst.resolve(seed, 'm', given=title)
    have = rows_of(res / 'build/pso_title.tsv') | rows_of(res / 'build/pso_manifest.tsv')
    pipelines = [r for r in title + rest if tuple(r) not in have]
    return {'game': game, 'version': version, 'list_sha256': hashlib.sha256(Path(listfile).read_bytes()).hexdigest(),
            'new_shaders': len(shaders), 'new_pipelines': len(pipelines),
            'waiting': counts_t['missing'] + counts_m['missing'], 'listed_rows': len(title) + len(rest)}


def say(text):
    print(text, flush=True)


def tools():
    """build_app and update_runtime (this checkout's), imported once."""
    if str(REPO) not in sys.path:
        sys.path.insert(0, str(REPO))
    import build_app
    import update_runtime
    return build_app, update_runtime


def seed_chain(seed):
    return json.loads((Path(seed) / 'seed-manifest.json').read_text()).get('chain')


def tree_chain(tree):
    """The translation chain of the work folder's copy of the layer (the one its translator and scripts belong to)."""
    import re
    text = (Path(tree) / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()
    m = re.search(r'#define DMSH_CHAIN "([^"]+)"\s*/\* game: totk\b', text) or re.search(r'#define DMSH_CHAIN "([^"]+)"', text)
    return m.group(1) if m else None


def can_translate(app, version, tree):
    """'' when the work folder can translate new shaders for this app, else why not (in words). It must hold the build's
    game files the seed reads (update_runtime.check_work_files: the app's own RomFS index and modules) and be of the
    app's translation chain: the work folder's translator and scripts are used as they are, never this checkout's."""
    ba, ur = tools()
    res = Path(app) / 'Contents/Resources/totk'
    if not Path(tree).is_dir():
        return f'the build\'s work folder ({tree}) is not there'
    try:
        ur.check_work_files(Path(tree), res / f'build/game-data/{version}', version)
    except ba.BuildError as e:
        return f'the work folder does not hold this app\'s game files: {e}'
    have, want = tree_chain(tree), seed_chain(res / 'build/shader-seed')
    if have != want:
        return f'the work folder translates for chain {have}, the app\'s shaders are of {want}: update the app first'
    return ''


def update(app, listfile, work=None):
    """The app with what the list adds (see the module's text). Returns the check's result with what was done."""
    ba, ur = tools()
    app = Path(app)
    result = check(app, listfile)
    game, version = result['game'], result['version']
    if not result['new_shaders'] and not result['new_pipelines']:
        say('nothing new for this app: its shaders and pipelines are those of the newest list')
        return {**result, 'done': 'nothing'}
    if game == 'oot':
        return {**result, **update_ocarina(app, listfile, work)}
    folder, why = (Path(work).expanduser().resolve(), 'given') if work else ba.recorded_work_folder(app, ur.old_work_name(game))
    tree, logs = folder / 'totk', folder / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    say(f'{result["new_shaders"]} new shaders, {result["new_pipelines"]} new pipelines; work folder {folder} ({why})')
    why_not = can_translate(app, version, tree) if result['new_shaders'] else 'no new shaders'
    translate = not why_not
    if result['new_shaders'] and not translate:
        say(f'   the new shaders are left out: {why_not}; only the new pipelines are made')
        if not result['new_pipelines']:
            return {**result, 'done': 'nothing', 'reason': why_not}
    if translate:
        say('translating the new shaders (the work folder\'s translator; shaders translated before are reused)')
        if game == 'botw':
            ba.step_shaders_botw(tree, folder / 'botw', logs, version=version, shader_list=listfile)
        else:
            ba.step_shaders(tree, logs, version=version, shader_list=listfile)
        ba.step_shader_libs(tree, logs, version=version)
    say(f'assembling the updated app next to {app.name}')
    tmp = app.with_name(app.name + '.partial')
    if tmp.exists():
        ba.remove_tree(tmp)
    bundle_id = plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier') or ba.APPS[game][1]
    shaders = None if translate else ba.app_shader_bundle(app, version)
    if shaders:
        ba.clone_app_except(app, tmp, shaders)
    else:
        ba.copy_tree(app, tmp)
    replaced = []
    try:
        new_res = tmp / 'Contents/Resources/totk'
        if translate:
            ur.replace_seed(tree, tmp, version, game, logs, bundle_id)
        ba.run([sys.executable, SCRIPTS / 'shader_list.py', 'resolve', new_res / 'build/shader-seed', listfile,
                new_res / 'build/pso_manifest.tsv', new_res / 'build/pso_title.tsv'], logs / 'shader-list-resolve.log')
        ba.mac_pipelines(tree, new_res, version, logs)
        if shaders:
            ba.shader_signature_only(tmp, shaders)
        ba.sign_app(tmp, logs)
        ba.wait_until_closed(app)
        ba.install_app(tmp, app, remove=replaced.append)
    finally:
        if tmp.exists():
            ba.remove_tree(tmp)
    ur.check_installed(app, game, logs, replaced)
    if translate:
        ba.step_warm_metal(tree, app, logs)
    after = check(app, listfile)
    say(f'done: {after["new_shaders"]} shaders and {after["new_pipelines"]} pipelines of the list still missing')
    return {**result, 'done': 'shaders and pipelines' if translate else 'pipelines', 'after': after,
            **({'reason': why_not} if result['new_shaders'] and not translate else {})}


def update_ocarina(app, listfile, work):
    """Ocarina of Time: its own build's update_shaders (games/oot/build.py), which translates with the work folder's
    game binary and packages the App."""
    import importlib.util
    ba, _ = tools()
    ba.codec_libraries()   # the prefixes its Makefile reads, as update_runtime.update_ocarina sets them
    spec = importlib.util.spec_from_file_location('oot_build', REPO / 'games/oot/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.update_shaders(app, listfile, work)
    after = check(app, listfile)
    say(f'done: {after["new_shaders"]} shaders and {after["new_pipelines"]} pipelines of the list still missing')
    return {'done': 'shaders and pipelines', 'after': after}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app', type=Path)
    ap.add_argument('--check', action='store_true', help='only compare: change nothing')
    ap.add_argument('--list', type=Path, help='this shader list instead of downloading the newest from GitHub')
    ap.add_argument('--work', type=Path, help='the build\'s work folder (default: the one the app records, MNGWorkFolder)')
    ap.add_argument('--json', action='store_true', help='print the result as one JSON line')
    a = ap.parse_args(argv)
    app = a.app.expanduser().resolve()
    try:
        game, _ = identity(app)
        with tempfile.TemporaryDirectory() as tmp:
            listfile = a.list or download(game, tmp)
            result = check(app, listfile) if a.check else update(app, listfile, a.work)
    except Exception as e:   # the reason, in words, for the window and the terminal
        print(json.dumps({'ok': False, 'error': str(e)}) if a.json else f'error: {e}')
        return 1
    if a.json:
        print(json.dumps({'ok': True, **result}))
    else:
        print(f"{result['game']} {result['version']}: {result['new_shaders']} new shaders, {result['new_pipelines']} new "
              f"pipelines ready to make, {result['waiting']} pipelines waiting for new shaders")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
