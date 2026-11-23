"""Programs a Mac build would compile, compiled once by the release from the same source (user 2026-11-17: "做 1": a player
needs no Xcode command line tools to package a game).

What a Mac build compiles is the repository's own code and pinned third-party sources, never anything of the game
(games/totk/Makefile: "The game binaries below are built from this repository alone and hold nothing of the game"):
the runtime of each game version, SPIRV-Cross, the warm-up tool and the update unpacker. The release compiles them in a
copy of the commit it ships and puts them in that source archive's .prebuilt folder with a manifest (the commit and each
file's SHA-256); the Launcher's sources and its signed code updates carry the folder. A build takes a program from it
only when the folder belongs to the source it runs from (the folder's commit is the source's .mng-source-commit) and the
file's digest is the manifest's; anything else compiles as before, so a checkout, or a source with local changes,
needs the command line tools as it always did. MNG_PREBUILT_ONLY=1 makes any compile that a prebuilt program should
have spared stop instead (tests: a build that runs with no compiler).

  python3 tools/prebuilt.py build <source folder>   compile every program into <source folder>/.prebuilt (the release)
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

FOLDER = '.prebuilt'
SCHEMA = 1
# Tears of the Kingdom 1.0.0 / 1.2.1 / 1.4.3, Breath of the Wild 1.6.0 / 1.9.0, Bananza, Pokopia (games/totk/Makefile)
TOTK_VERSIONS = ('v0', 'v121', 'v143', 'botw160', 'botw190', 'dkb', 'pkp')


def runtime_name(version):
    """The program games/totk/Makefile makes for a version (build/<name>), and its name in the folder."""
    return 'totk-native' if version == 'v0' else f'totk-native-{version}'


# Ocarina of Time's runtime and its pool sources reader (games/oot/Makefile: made from the repository alone, nothing of
# the game; user 2026-11-22: "时之笛的运行时改成发布时编好"). Its build still compiles the game's shader libraries with
# Xcode's Metal compiler (classify.oot_needs_metal): those are made from the player's game files.
OOT_PROGRAMS = {'oot-native': 'oot-native', 'oot-pool-sources-list': 'pool-sources-list'}   # name here: name in build/
NAMES = (*(runtime_name(v) for v in TOTK_VERSIONS), 'spirv-cross', 'warm-metal-cache', 'hactool', *OOT_PROGRAMS)


class Missing(Exception):
    """A compile MNG_PREBUILT_ONLY=1 forbids."""


def only():
    return os.environ.get('MNG_PREBUILT_ONLY') == '1'


def commit_of(source):
    try:
        return (Path(source) / '.mng-source-commit').read_text().strip() or None
    except OSError:
        return None


def manifest(source):
    """The folder's manifest when it belongs to this source (same commit), else None."""
    source = Path(source)
    try:
        data = json.loads((source / FOLDER / 'manifest.json').read_text())
    except (OSError, ValueError):
        return None
    commit = commit_of(source)
    if data.get('schema') != SCHEMA or not commit or data.get('commit') != commit:
        return None
    return data


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def program(source, name):
    """The prebuilt program `name` of this source, its digest checked, or None."""
    data = manifest(source)
    entry = (data or {}).get('files', {}).get(name)
    path = Path(source) / FOLDER / name
    if not entry or not path.is_file() or _sha256(path) != entry.get('sha256'):
        return None
    return path


def place(source, name, dest, why=''):
    """Put the prebuilt `name` at dest (executable) and say so; False when there is none, so the caller compiles. With
    MNG_PREBUILT_ONLY=1 a missing one raises Missing instead."""
    path = program(source, name)
    if path is None:
        if only():
            raise Missing(f'{name}: no prebuilt program for this source (MNG_PREBUILT_ONLY=1){": " + why if why else ""}')
        return False
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.prebuilt-partial')
    shutil.copy2(path, tmp)
    os.chmod(tmp, 0o755)
    os.replace(tmp, dest)
    print(f'   {name}: the release\'s prebuilt program (no compile)', flush=True)
    return True


def keep_placed(source, name, dest):
    """place(), but a dest already holding the same bytes is left as it is (its time too, so what make builds from it
    is not made again); True when the release's program is there."""
    path = program(source, name)
    if path is None:
        return place(source, name, dest)
    dest = Path(dest)
    if dest.is_file() and _sha256(dest) == _sha256(path):
        print(f'   {name}: the release\'s prebuilt program (already in place)', flush=True)
        return True
    return place(source, name, dest)


def complete(source):
    """Every program is there for this source: a build of any game needs no compiler (build_app.prerequisites)."""
    return all(program(source, n) is not None for n in NAMES)


def compiler_needed(what):
    """A compile is about to run: allowed, unless MNG_PREBUILT_ONLY=1."""
    if only():
        raise Missing(f'{what}: would compile, which MNG_PREBUILT_ONLY=1 forbids')


def build(source):
    """Compile every program in `source` (a disposable copy of the commit: its games/ get build folders) into
    source/.prebuilt, with the manifest. Needs the command line tools; the release runs it."""
    source = Path(source).resolve()
    commit = commit_of(source)
    if not commit:
        raise SystemExit(f'{source}: no .mng-source-commit; build in a copy of the commit the release ships')
    sys.path.insert(0, str(source))
    os.environ['MNG_REPO'] = str(source)
    import build_app   # this source's own build code
    out = source / FOLDER
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    logs = Path(tempfile.mkdtemp(prefix='mng-prebuilt-logs-'))
    jobs = str(os.cpu_count() or 4)
    totk = source / 'games/totk'
    build_app.codec_libraries('macos')   # opus, libvpx, dav1d the runtimes link (tools/deps)
    env = dict(os.environ)
    # checked in, made from the runtime sources: a fresh checkout's file times must not ask make to make it again (that
    # needs the build's Python environment)
    (totk / 'runtime/botw_pchtxt_sites.h').touch()
    for version in TOTK_VERSIONS:
        name = runtime_name(version)
        build_app.run(['make', '-C', totk, f'-j{jobs}', f'build/{name}'], logs / f'{name}.log', env=env)
        shutil.copy2(totk / 'build' / name, out / name)
    build_app.build_spirv_cross(totk, logs)   # fetches the pinned sources into totk/tooling (git)
    shutil.copy2(totk / 'build/spirv-cross/spirv-cross', out / 'spirv-cross')
    build_app.build_warm_tool(totk, logs, env)
    shutil.copy2(totk / 'build/warm-metal-cache', out / 'warm-metal-cache')
    shutil.copy2(build_app.build_hactool(totk, logs), out / 'hactool')
    oot_work = Path(tempfile.mkdtemp(prefix='mng-prebuilt-oot-'))   # make's WORK: outside the source, with no space
    build_app.run(['make', '-C', source / 'games/oot', f'-j{jobs}', f'WORK={oot_work}', 'runtime'], logs / 'oot.log', env=env)
    for name, made in OOT_PROGRAMS.items():
        shutil.copy2(oot_work / 'build' / made, out / name)
    shutil.rmtree(oot_work, ignore_errors=True)
    files ={n: {'sha256': _sha256(out / n), 'bytes': (out / n).stat().st_size} for n in NAMES}
    (out / 'manifest.json').write_text(json.dumps({'schema': SCHEMA, 'commit': commit, 'files': files}, indent=1) + '\n')
    print(f'{len(files)} prebuilt programs for {commit[:12]} in {out}', flush=True)
    return out


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    else:
        raise SystemExit(__doc__)
