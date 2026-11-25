"""Make and check the Launcher's offline store (tools/deps/offline.py): everything a build of any game the Launcher makes
would download, so a player needs no network (user 2026-11-20; most players are in China, where GitHub is blocked).

    python3 -m tools.deps.offline_pack build <store>   at a release, online: fills <store> (files/, wheels/, git/,
                                                       sources/, manifest.json); what this Mac already has is reused
    python3 -m tools.deps.offline_pack check <store>   with the network off (denied by check itself, NO_NETWORK): from an
                                                       empty deps cache, takes every item from the store and makes it;
                                                       records the manifest it passed for in <store>/.checked.json
A release (MNG-Launcher make_app.py) carries a store only when `check` passed for its very manifest (checked()), so
the steps are: build (online), check (offline), then the release, which builds again (nothing changes) and checks the
record.

What goes in is read from the code that downloads it, not listed twice: tools/deps/pins.py (sources, packages, fonts,
build tools), the games' requirements.txt, the git commits the build scripts pin (git_pins reads them) and the app icons
the games pin (games/*/assets/app-icon.json); not what no game the Launcher builds uses (offline.NOT_CARRIED: the .NET
SDK, LibHac). tests/test_offline_store.py checks that every download in a build script is one of them.
The manifest has no date, so building again with nothing new leaves it (and the check record) valid.
Python 3.10 or later (run it with the build's Python 3.14)."""
import argparse
import ast
import errno
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from tools.deps import BUILD_TOOLS, SOURCES, offline  # noqa: E402
from tools.deps.core import digest, fetch  # noqa: E402
from tools.deps.pins import FONTTOOLS, OPEN_FONTS, OPEN_FONTS_LICENSE, PACKAGES  # noqa: E402

# Every game the Launcher builds (tools/builder_jobs/repo.py GAMES) installs one of these with pip; Super Mario Odyssey
# names its packages in games/odyssey/build.py PACKAGES.
REQUIREMENTS = ('games/totk', 'games/oot', 'games/acnh', 'games/dread', 'games/eow', 'games/tmnt')
ICONS_FONTTOOLS = 'tools/builder/icons_env.py'   # the controller-icons page's fontTools (VERSION)
PYTHON = '3.14'
PLATFORM = 'macosx_14_0_arm64'
# GPL code the store carries as a program: its source goes beside it (sources/)
GPL_SOURCES = ('zbic==1.0.1.post1',)
CHECKED = '.checked.json'   # check's record: the manifest it passed for, with the network off
# The check denies the network itself (user 2026-11-25 chose this when the storage guard was retired; until then a
# person had to run it under that guard's --offline, it wrote its record with the network on just the same, and a Mac
# where the guard refuses every command could not make a release). The rule is the guard's: every outbound connection
# but this Mac's loopback. macOS's sandbox-exec applies it to check and everything check starts.
SANDBOX = '/usr/bin/sandbox-exec'
NO_NETWORK = ('(version 1)\n(allow default)\n(deny network-outbound (remote ip))\n'
              '(allow network-outbound (remote ip "localhost:*"))')
INSIDE = 'MNG_OFFLINE_CHECK_NO_NETWORK'   # set for the run under NO_NETWORK; the proof is network_denied(), not this
PROBE = ('192.0.2.1', 443)   # TEST-NET-1, an address no host has: nothing is contacted when the network is on


def network_denied(connect=None):
    """True when an outbound connection is refused by the system (EPERM, what NO_NETWORK does). An address that cannot be
    reached, a timeout or a connection that succeeds is not that: no Wi-Fi proves nothing about a build."""
    connect = connect or (lambda: socket.create_connection(PROBE, timeout=2).close())
    try:
        connect()
    except OSError as error:
        return isinstance(error, PermissionError) or error.errno == errno.EPERM
    return False


def without_network(argv):
    return [SANDBOX, '-p', NO_NETWORK, *map(str, argv)]


def constant(path, name):
    """A module-level literal of a Python file, read without importing it."""
    for node in ast.parse((REPO / path).read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise KeyError(f'{path}: no {name}')


def git_pins():
    """[(url, commit)]: every commit a build fetches with git."""
    pins = [(url, pin) for name, (_, url, pin) in SOURCES.items()
            if url.endswith('.git') and name not in offline.NOT_CARRIED and name not in built_names()]
    pins += [constant('build_app.py', 'HACTOOL'), constant('build_app.py', 'SPIRV_CROSS'),
             constant('games/acnh/tools/build_hactool.py', 'HACTOOL'),
             constant('games/odyssey/scripts/build_odyssey.py', 'SPIRV_CROSS')]
    make = (REPO / 'games/eow/Makefile').read_text()
    pins.append((re.search(r'git clone -q (https://\S+)', make).group(1),
                 re.search(r'^SPIRV_CROSS_COMMIT\s*:?=\s*([0-9a-f]{40})', make, re.M).group(1)))
    fps = constant('games/acnh/build.py', 'FPS_SOURCE_URL')
    pins += [(fps, c) for c in re.findall(r'"commit":\s*"([0-9a-f]{40})"', (REPO / 'games/acnh/tools/prepare_fps.py').read_text())]
    return sorted(set(pins))


def repo_name(url):
    owner, name = url.rstrip('/').removesuffix('.git').split('/')[-2:]
    return f'{owner}-{name}'


def built_names():
    """The tools/deps libraries and programs the store carries made (built/), not as sources: every pinned source but
    what is not carried (user 2026-11-22: "做": a player's Mac compiles none of them)."""
    return [name for name in SOURCES if name not in offline.NOT_CARRIED]


def files_wanted():
    """{digest: (url, what)}: every pinned download fetched by its digest (not offline.NOT_CARRIED, nor the sources of
    what is carried built)."""
    from tools.deps.open_fonts import _url
    out = {pin: (url, f'{name} {version}') for name, (version, url, pin) in SOURCES.items()
           if not url.endswith('.git') and name not in offline.NOT_CARRIED and name not in built_names()}
    out.update({d: (url, filename) for name, (filename, url, d) in PACKAGES.items() if name not in offline.NOT_CARRIED})
    out[FONTTOOLS[2]] = (FONTTOOLS[1], FONTTOOLS[0])
    out.update({d: (_url(path), path) for _, _, path, d in OPEN_FONTS})
    out[OPEN_FONTS_LICENSE[1]] = (_url(OPEN_FONTS_LICENSE[0]), OPEN_FONTS_LICENSE[0])
    for spec in sorted(REPO.glob('games/*/assets/app-icon.json')):
        icon = json.loads(spec.read_text())
        out[icon['sha256']] = (icon['url'], f'{spec.parent.parent.name} app icon')
    return out


def requirement_sets():
    """[(label, [pip requirement lines])]: each set is resolved on its own (two games may pin one package differently)."""
    sets = [(f'{folder}/requirements.txt', [l.split('#')[0].strip() for l in (REPO / folder / 'requirements.txt').read_text().splitlines()
                                            if l.split('#')[0].strip()]) for folder in REQUIREMENTS]
    sets.append(('games/odyssey/build.py PACKAGES', list(constant('games/odyssey/build.py', 'PACKAGES'))))
    sets.append(('tools/deps/pins.py BUILD_TOOLS', [f'{n}=={v}' for n, (v, _) in BUILD_TOOLS.items() if n not in offline.NOT_CARRIED]))
    sets.append((ICONS_FONTTOOLS, [f"fonttools=={constant(ICONS_FONTTOOLS, 'VERSION')}"]))
    return sets


def say(text):
    print(f'[offline] {text}', flush=True)


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f'{" ".join(map(str, cmd[:6]))} … failed ({r.returncode}):\n' + (r.stdout + r.stderr)[-3000:])
    return r.stdout


# ---- build ----
def build_files(store):
    from tools.deps.core import cache
    folder = store / 'files'
    folder.mkdir(parents=True, exist_ok=True)
    kind = lambda want: 'sha512' if len(want) == 128 else 'sha256'
    have = {}   # what this Mac's deps cache already downloaded, by digest (its own names differ from call to call)
    for path in (cache() / 'src').glob('*'):
        if path.is_file():
            have[digest(path)] = path
            if path.name.endswith('.tar.gz') and path.stat().st_size > 100e6:   # the .NET SDK is pinned by SHA-512
                have[digest(path, 'sha512')] = path
    wanted = files_wanted()
    for want, (url, what) in wanted.items():
        target = folder / want
        if not (target.is_file() and digest(target, kind(want)) == want):
            source = have.get(want) or fetch(url, want, f'offline-{want[:16]}')
            shutil.copyfile(source, target)
            if digest(target, kind(want)) != want:
                raise RuntimeError(f'{what}: the copy does not match its pin')
        say(f'file  {what} ({target.stat().st_size / 1e6:.1f} MB)')
    for old in folder.iterdir():   # a pin that changed, or is no longer carried (the .NET SDK until 2026-11-21)
        if old.name not in wanted:
            say(f'file  removed {old.name[:16]} (no longer pinned)')
            old.unlink()


def build_git(store):
    repos = {}
    for url, commit in git_pins():
        repos.setdefault(repo_name(url), {'urls': set(), 'commits': set()})
        repos[repo_name(url)]['urls'].add(url)
        repos[repo_name(url)]['commits'].add(commit)
    for name, r in sorted(repos.items()):
        bare = store / 'git' / f'{name}.git'
        if not bare.is_dir():
            run(['git', 'init', '-q', '--bare', bare])
        for commit in sorted(r['commits']):
            have = subprocess.run(['git', '-C', bare, 'cat-file', '-e', f'{commit}^{{commit}}'], capture_output=True)
            if have.returncode:
                url = sorted(r['urls'])[0]
                run(['git', '-C', bare, 'fetch', '-q', '--depth', '1', url, f'{commit}:refs/tags/pin-{commit}'])
        first = sorted(r['commits'])[0]
        run(['git', '-C', bare, 'update-ref', 'refs/heads/main', first])
        run(['git', '-C', bare, 'symbolic-ref', 'HEAD', 'refs/heads/main'])
        say(f'git   {name}: {", ".join(c[:12] for c in sorted(r["commits"]))}')
    for old in (store / 'git').glob('*.git'):   # a repository no longer pinned, or not carried (glslang, 2026-11-22)
        if old.name.removesuffix('.git') not in repos:
            say(f'git   removed {old.name}')
            shutil.rmtree(old)
    return {name: sorted(r['urls']) for name, r in repos.items()}


def build_wheels(store, python):
    """wheels/ holds exactly what the requirement sets resolve to: made in a new folder (the last one's wheels offered
    with --find-links, so nothing unchanged is downloaded again), then put in place of the old one."""
    folder, fresh = store / 'wheels', store / 'wheels.new'
    folder.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(fresh, ignore_errors=True)
    fresh.mkdir()
    for label, lines in requirement_sets():
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write('\n'.join(lines) + '\n')
        try:
            run([python, '-m', 'pip', 'download', '--disable-pip-version-check', '--only-binary=:all:', '--platform', PLATFORM,
                 '--python-version', PYTHON, '--implementation', 'cp', '--abi', 'cp' + PYTHON.replace('.', ''), '-d', fresh,
                 '--find-links', folder, '-r', f.name])
        finally:
            os.unlink(f.name)
        say(f'pip   {label}: {", ".join(lines)}')
    shutil.rmtree(folder)
    fresh.rename(folder)
    sources = store / 'sources'
    sources.mkdir(exist_ok=True)
    for spec in GPL_SOURCES:   # GPL: the program's source goes with it
        run([python, '-m', 'pip', 'download', '--disable-pip-version-check', '--no-binary=:all:', '--no-deps', '-d', sources, spec])
        say(f'source {spec} (GPL)')


def module(name, path):
    """A game's script as a module of its own name, its folder on sys.path."""
    import importlib.util
    folder = str((REPO / path).parent)
    if folder not in sys.path:
        sys.path.insert(0, folder)
    spec = importlib.util.spec_from_file_location(name, REPO / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


LICENCE_FILES = ('LICENSE', 'LICENCE', 'COPYING', 'PATENTS', 'NOTICE')


def licences(name):
    """The licence files of a pinned source (its top folder, or the one folder inside an archive)."""
    from tools.deps import source
    root = Path(source(name))
    inside = [p for p in root.iterdir() if p.is_dir() and not p.name.startswith('.')]
    roots = [root] + (inside if len(inside) == 1 else [])
    return [p for r in roots for p in sorted(r.iterdir()) if p.is_file() and p.name.upper().startswith(LICENCE_FILES)]


RECIPES = ('tools/deps/__init__.py', 'tools/deps/core.py', 'tools/deps/pins.py')   # what says how each is made


def recipe_digest():
    return digest_of_files(REPO / p for p in RECIPES)


def digest_of_files(paths):
    import hashlib
    h = hashlib.sha256()
    for path in paths:
        h.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
    return h.hexdigest()


def build_built(store, old):
    """({folder: {file: SHA-256}}, recipe digest): each of built_names() as tools/deps makes it on any Mac (prefix()),
    copied into built/<its cache folder> with its licences (licences/). Made in an empty deps cache, so this Mac's
    older builds are never taken; kept from the last store when the recipes (RECIPES) are the same and its files are
    intact, so building the store again changes nothing (a compile is not byte for byte the same twice)."""
    from tools.deps import _folder
    folder = store / 'built'
    folder.mkdir(exist_ok=True)
    recipes = recipe_digest()
    wanted = {_folder(name): name for name in built_names()}
    kept = old.get('built') or {}
    if old.get('built_recipes') == recipes and set(kept) == set(wanted) and all(
            (folder / f).is_dir() and offline.tree_files(folder / f) == files for f, files in kept.items()):
        say(f'built {", ".join(sorted(kept))}: kept (same recipes {recipes[:12]})')
        return kept, recipes
    out = {}
    with tempfile.TemporaryDirectory() as t:
        before = os.environ.get('MNG_DEPS_CACHE')
        os.environ['MNG_DEPS_CACHE'] = str(Path(t) / 'cache')
        try:
            from tools.deps import prefix
            for target_name, name in wanted.items():
                made, target = prefix(name), folder / target_name
                shutil.rmtree(target, ignore_errors=True)
                shutil.copytree(made, target, symlinks=True)
                found = licences(name)
                if not found:
                    raise RuntimeError(f'{name}: no licence file in its source; a built copy needs one beside it')
                (target / 'licences').mkdir(exist_ok=True)
                for path in found:
                    shutil.copy2(path, target / 'licences' / path.name)
                out[target_name] = offline.tree_files(target)
                size = sum(p.stat().st_size for p in target.rglob('*') if p.is_file())
                say(f'built {target_name} ({size / 1e6:.1f} MB; {", ".join(p.name for p in found)})')
        finally:
            if before is None:
                os.environ.pop('MNG_DEPS_CACHE', None)
            else:
                os.environ['MNG_DEPS_CACHE'] = before
    for stale in folder.iterdir():
        if stale.name not in out:
            shutil.rmtree(stale)
    return out, recipes


def build(store, python):
    if offline.store() is not None:   # prefix() would take the built copies from that store instead of making them
        raise RuntimeError(f'{offline.STORE_ENV} is set: build a store where no store is in use')
    store.mkdir(parents=True, exist_ok=True)
    for old in ('nuget', 'dotnet-tools'):   # what the store held on 2026-11-21, no longer carried
        shutil.rmtree(store / old, ignore_errors=True)
    build_files(store)
    git = build_git(store)
    build_wheels(store, python)
    try:
        old = json.loads((store / offline.MANIFEST).read_text())
    except (OSError, ValueError):
        old = {}
    made, recipes = build_built(store, old)
    data = {'schema': offline.SCHEMA, 'git': git,   # no date: the same contents make the same manifest (CHECKED)
            'files': {d: what for d, (_, what) in files_wanted().items()},
            'wheels': sorted(p.name for p in (store / 'wheels').glob('*.whl')), 'built': made, 'built_recipes': recipes}
    (store / offline.MANIFEST).write_text(json.dumps(data, indent=1, sort_keys=True) + '\n')
    size = sum(p.stat().st_size for p in store.rglob('*') if p.is_file())
    say(f'store {store}: {size / 1e6:.0f} MB')


# ---- check (network off) ----
def check(store, python):
    """Every item from the store alone, in an empty deps cache: raises on the first one that is missing."""
    store = store.resolve()
    if not network_denied():
        raise RuntimeError('the check proves nothing with the network on: run `python3 -m tools.deps.offline_pack check '
                           '<store>`, which denies the network itself')
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        env = offline.environment(store, dict(os.environ, MNG_DEPS_CACHE=str(tmp / 'cache')))
        env.pop('MNG_OFFLINE_ACTIVE', None)
        missing = [what for d, what in json.loads((store / offline.MANIFEST).read_text())['files'].items()
                   if not (store / 'files' / d).is_file()]
        if missing:
            raise RuntimeError(f'not in the store: {", ".join(missing)}')
        # every pinned library and program, made from its source (tools/deps all), and the open fonts
        run([python, '-B', '-m', 'tools.deps', 'all'], env=env, cwd=REPO)
        say('deps  every pinned library and program made from the store')
        run([python, '-B', '-m', 'tools.deps', 'fonts'], env=env, cwd=REPO)
        say('deps  open fonts made from the store')
        for i, (label, lines) in enumerate(requirement_sets()):
            venv = tmp / f'venv-{i}'
            run([python, '-m', 'venv', venv], env=env)
            wanted = tmp / f'req-{i}.txt'
            wanted.write_text('\n'.join(lines) + '\n')
            run([venv / 'bin/python', '-m', 'pip', 'install', '-q', '-r', wanted], env=env)
            say(f'pip   {label}')
        icons = tmp / 'icons'
        run([python, '-m', 'venv', icons], env=env)
        sys.path.insert(0, str(REPO))
        from tools.builder import icons_env
        wanted = tmp / 'icons-req.txt'
        wanted.write_text(icons_env.requirements())
        os.environ['MNG_OFFLINE_STORE'] = str(store)
        run(icons_env.pip_argv(icons, wanted), env=icons_env.pip_env())
        say('pip   the controller-icons page (fontTools, hash-pinned)')
        for url, commit in git_pins():
            a, b = tmp / f'git-a-{commit[:12]}', tmp / f'git-b-{commit[:12]}'
            run(['git', 'init', '-q', a], env=env)
            run(['git', '-C', a, 'fetch', '-q', '--depth', '1', url, commit], env=env)
            run(['git', '-C', a, 'checkout', '-q', 'FETCH_HEAD'], env=env)
            run(['git', 'clone', '-q', url, b], env=env)
            run(['git', '-C', b, 'checkout', '-q', '--detach', commit], env=env)
            for folder in (a, b):
                if run(['git', '-C', folder, 'rev-parse', 'HEAD'], env=env).strip() != commit:
                    raise RuntimeError(f'{url} {commit}: another commit checked out')
            say(f'git   {url} {commit[:12]} (fetch and clone)')
        not_carried(python, env, tmp)
    record = {'manifest_sha256': digest(store / offline.MANIFEST), 'at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    (store / CHECKED).write_text(json.dumps(record) + '\n')
    say('check passed: nothing was downloaded')


def not_carried(python, env, tmp):
    """What the store leaves out is refused, not downloaded; TMNT's translator, the one .NET program a Launcher game
    runs, is its prebuilt NativeAOT file and starts with no .NET at all."""
    for name in offline.NOT_CARRIED:   # a package (LibHac) has no command of its own
        command = 'prefix' if name in SOURCES else 'program' if name in BUILD_TOOLS else None
        if command is None:
            continue
        r = subprocess.run([python, '-B', '-m', 'tools.deps', command, name], env=env, cwd=REPO, capture_output=True,
                           text=True, timeout=600)
        if r.returncode == 0:
            raise RuntimeError(f'{name}: made with the store in use ({(r.stdout + r.stderr)[-300:]})')
        say(f'deps  {name}: not carried, cannot be made')
    tmnt = module('tmnt_build_shader_tools', 'games/tmnt/scripts/build_shader_tools.py')
    _, service = tmnt.build(tmp / 'tmnt')
    r = subprocess.run([str(service), '--serve'], input='', capture_output=True, text=True, timeout=60,
                       env={'PATH': '/usr/bin:/bin', 'HOME': str(tmp)})   # no DOTNET_ROOT, no SDK on PATH
    if r.returncode:
        raise RuntimeError(f'TMNT\'s prebuilt translator does not start (exit {r.returncode}): {(r.stderr or r.stdout)[-500:]}')
    say('.NET  TMNT\'s prebuilt translator starts with no .NET (NativeAOT)')


def checked(store):
    """True when `check` passed with the network off for exactly this manifest (a release carries only such a store)."""
    try:
        record = json.loads((Path(store) / CHECKED).read_text())
    except (OSError, ValueError):
        return False
    return record.get('manifest_sha256') == digest(Path(store) / offline.MANIFEST)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('action', choices=('build', 'check'))
    ap.add_argument('store', type=Path)
    ap.add_argument('--python', default=sys.executable, help='a Python 3.14 with pip (default: this one)')
    a = ap.parse_args(argv)
    store = a.store.expanduser().resolve()
    if a.action == 'check' and os.environ.get(INSIDE) != '1':   # again, with the network denied
        return subprocess.call(without_network([sys.executable, '-B', '-m', 'tools.deps.offline_pack', 'check', store,
                                                '--python', a.python]), env=dict(os.environ, **{INSIDE: '1'}), cwd=REPO)
    try:
        (build if a.action == 'build' else check)(store, a.python)
    except (RuntimeError, OSError, KeyError, ValueError, subprocess.TimeoutExpired) as e:
        print(f'[offline] FAILED: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
