#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

OT0 = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(OT0 / 'scripts'))
import versions

APP_NAME = 'Octopath Traveler 0'
MODULES = ('sdk', 'subsdk0', 'main')
REWRITE_FILES = ('patched.bin', 'fixups.tsv', 'gates.tsv', 'cuts.tsv', 'obs.tsv', 'params.tsv', 'sites.tsv')
LIBRARIES = {'opus': 'lib/libopus.a', 'libvpx': 'lib/libvpx.a'}
APP_FILESYSTEMS = ('apfs', 'hfs')
WORK_UNSAFE = ':$"'
MAKE_UNSAFE = '$"'
HEADROOM = 2 << 30
HELP = f"""Build {APP_NAME} for Apple silicon from your own game files, in one command.

    cd games/ot0
    python3.14 scripts/build_ot0.py --game <card image or base game> --update <1.0.3 or 1.0.4 update.nsp> -k <prod.keys>
                                    [--work <folder>] [--out <folder>]

Steps (a second run reuses every finished one):
  1. code: the update's ExeFS, identified as {versions.supported()} by its hashes (never by the file name)
  2. data: the RomFS merged from the game file and the update, all six IVFC levels verified
  3. rewrite: every module rewritten for Apple silicon (third_party/d2r), checked against assets/expected-<version>.json
  4. runtime: make prepare runtime shader-tools (the tables, the native host, the NVN-on-Metal layer, the shader tools)
  5. app: "{APP_NAME}.app" next to the game file (or in --out), which carries everything it runs with
The first build needs internet access (Python packages, SPIRV-Cross) and about 10 GB free for the work folder
(OT0_build by default; git ignores it), whose path must not contain spaces, : or $. The app must go on an APFS or Mac OS
Extended volume. One app holds one version:
building the other version into the same folder replaces it, so give that one another --out to keep both.
Game data stays in the work folder and in the app; never share either. Saves, the shader cache, mods and the log live in
~/Library/Application Support/MacNativeGames/OT0 and ~/Library/Logs/MacNativeGames/ot0.log."""


class BuildError(Exception):
    pass


def say(message):
    print(f'[{time.strftime("%H:%M:%S")}] {message}', flush=True)


def run(cmd, log, env=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w') as f:
        code = subprocess.run([str(c) for c in cmd], cwd=OT0, env=env, stdout=f, stderr=subprocess.STDOUT).returncode
    if code:
        tail = '\n'.join(log.read_text(errors='replace').splitlines()[-20:])
        raise BuildError(f'{log.stem} failed (exit {code}); full output: {log}\n{tail}')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def existing(path):
    while not path.exists():
        path = path.parent
    return path


def filesystem(path):
    mount = existing(path).resolve()
    while not os.path.ismount(mount):
        mount = mount.parent
    table = subprocess.run(['mount'], capture_output=True, text=True).stdout
    found = re.search(rf'^\S.* on {re.escape(str(mount))} \(([^,)]+)', table, re.M)
    return found.group(1) if found else 'unknown'


def unsafe(path, chars, spaces):
    return sorted({c for c in str(path) if c in chars or (spaces and c.isspace())})


def largest_romfs():
    return max(pinned['romfs']['size'] for pinned in versions.VERSIONS.values())


def check_setup(work, out, inputs):
    if sys.version_info < (3, 14):
        raise BuildError(f'needs Python 3.14 (this is {sys.version.split()[0]}): start the build with python3.14')
    if subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode:
        raise BuildError("Xcode's Command Line Tools are missing: run xcode-select --install")
    missing = [name for name, lib in LIBRARIES.items() if not (Path('/opt/homebrew/opt') / name / lib).is_file()]
    if missing:
        raise BuildError(f'missing Homebrew packages: run brew install {" ".join(missing)}')
    bad = unsafe(work, WORK_UNSAFE, spaces=True)
    if bad:
        raise BuildError(f'the work folder\'s path has {" ".join(repr(c) for c in bad)}, which make cannot handle: {work}\n'
                         f'choose another with --work (for example --work ~/Games/OT0_build)')
    for path in (out, *inputs):
        bad = unsafe(path, MAKE_UNSAFE, spaces=False)
        if bad:
            raise BuildError(f'{path} has {" ".join(repr(c) for c in bad)}, which make cannot pass on: rename or move it')
    kind = filesystem(out)
    if kind not in APP_FILESYSTEMS:
        raise BuildError(f'{existing(out)} is on a volume formatted as {kind}, where macOS cannot sign the app: '
                         f'choose a folder on your Mac\'s disk with --out')
    if os.stat(existing(out)).st_dev != os.stat(existing(work)).st_dev:
        need = largest_romfs() + (1 << 30)
        if shutil.disk_usage(existing(out)).free < need:
            raise BuildError(f'{existing(out)} is on another volume than the work folder, so the app is a full copy there: '
                             f'it needs about {need / 2**30:.0f} GB free')


def environment(work, logs):
    py = work / 'venv/bin/python'
    requirements = (OT0 / 'requirements.txt').read_text()
    stamp = work / 'venv/requirements.txt'
    if not py.exists() or not stamp.is_file() or stamp.read_text() != requirements:
        say('1/5 code: installing the Python packages the scripts need (once)')
        shutil.rmtree(work / 'venv', ignore_errors=True)
        run([sys.executable, '-m', 'venv', work / 'venv'], logs / 'venv.log')
        run([py, '-m', 'pip', 'install', '-q', '-r', OT0 / 'requirements.txt'], logs / 'pip.log')
        stamp.write_text(requirements)
    return py


def exefs_matches(exefs, version):
    pinned = versions.expected(version)['exefs_sha256']
    return all((exefs / name).is_file() and sha256(exefs / name) == sha for name, sha in pinned.items())


def code(py, update, keys, work, logs):
    staging = work / '.identify'
    shutil.rmtree(staging, ignore_errors=True)
    say('1/5 code: extracting the update\'s ExeFS and identifying its version')
    try:
        run([py, 'scripts/extract_code.py', update, '--keys', keys, '--work', staging], logs / 'extract_code.log')
        found = [d for d in (staging / 'extracted').iterdir() if d.name in versions.VERSIONS]
        if len(found) != 1:
            raise BuildError(f'extract_code.py wrote no version folder under {staging / "extracted"}')
        version = found[0].name
        target = work / f'extracted/{version}'
        if exefs_matches(target / 'exefs', version):
            say(f'1/5 code: {versions.expected(version)["display_version"]}, already extracted')
        else:
            shutil.rmtree(target / 'exefs', ignore_errors=True)
            target.mkdir(parents=True, exist_ok=True)
            (found[0] / 'exefs').rename(target / 'exefs')
            (found[0] / 'extraction-manifest.json').replace(target / 'extraction-manifest.json')
            say(f'1/5 code: {versions.expected(version)["display_version"]}')
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return version


def romfs_matches(romfs, version):
    pinned = versions.expected(version)['romfs']
    image, index = romfs / 'romfs.bin', romfs / 'index.json'
    if not image.is_file() or not index.is_file() or image.stat().st_size != pinned['size']:
        return False
    report = json.loads(index.read_text())
    return report.get('status') == 'pass' and report.get('romfs_sha256') == pinned['sha256'] and sha256(image) == pinned['sha256']


def data(py, game, update, keys, work, version, logs):
    romfs = work / f'extracted/{version}/romfs'
    if romfs_matches(romfs, version):
        return
    shutil.rmtree(romfs, ignore_errors=True)
    need = versions.expected(version)['romfs']['size'] + HEADROOM
    if shutil.disk_usage(existing(work)).free < need:
        raise BuildError(f'merging the {versions.expected(version)["display_version"]} RomFS needs about '
                         f'{need / 2**30:.0f} GB free in {existing(work)}')
    say('2/5 data: merging the RomFS with the update and verifying it')
    run([py, 'scripts/merge_romfs.py', game, update, '--keys', keys, '--work', work], logs / 'merge_romfs.log')
    if not romfs_matches(romfs, version):
        raise BuildError(f'the merged RomFS does not match assets/{versions.FILES[version]}; see {logs / "merge_romfs.log"}')


def rewrite_hashes(rewritten, module):
    files = [rewritten / f'{module}.{suffix}' for suffix in REWRITE_FILES]
    return {f.name: sha256(f) for f in files} if all(f.is_file() for f in files) else None


def rewrite(py, work, version, logs):
    pinned = versions.expected(version)['rewritten_sha256']
    rewritten = work / f'rewritten/{version}'
    rewritten.mkdir(parents=True, exist_ok=True)
    (work / 'state').mkdir(exist_ok=True)
    for module in MODULES:
        out = rewritten / f'{module}.patched.bin'
        stamp = work / f'state/rewrite-{version}-{module}.json'
        current = rewrite_hashes(rewritten, module)
        if current and current[out.name] == pinned[out.name] and stamp.is_file() and json.loads(stamp.read_text()) == current:
            continue
        say(f'3/5 rewrite: {module} for Apple silicon' + (' (a minute or two)' if module == 'main' else ''))
        run([py, 'third_party/d2r/patch_nso.py', work / f'extracted/{version}/exefs/{module}', rewritten, module],
            logs / f'rewrite-{module}.log', dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0'))
        current = rewrite_hashes(rewritten, module)
        if not current or current[out.name] != pinned[out.name]:
            raise BuildError(f'the rewrite of {module} does not match assets/{versions.FILES[version]}; '
                             f'see {logs / f"rewrite-{module}.log"}')
        stamp.write_text(json.dumps(current, indent=2) + '\n')


def elapsed(seconds):
    return f'{seconds / 60:.0f} min' if seconds >= 90 else f'{seconds:.0f} s'


def main(argv):
    parser = argparse.ArgumentParser(description=HELP, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', type=Path, required=True, help='the card image (XCI) or the base game (NSP)')
    parser.add_argument('--update', type=Path, required=True, help=f'the update (NSP): {versions.supported()}')
    parser.add_argument('-k', '--keys', type=Path, required=True, help='prod.keys from your own Switch')
    parser.add_argument('--work', type=Path, default=OT0 / 'OT0_build', help='the work folder (default: OT0_build here)')
    parser.add_argument('--out', type=Path, help='folder for the app (default: next to the game file)')
    a = parser.parse_args(argv)
    game, update, keys = (p.expanduser().absolute() for p in (a.game, a.update, a.keys))
    for path in (game, update, keys):
        if not path.is_file():
            raise BuildError(f'no such file: {path}')
    work = a.work.expanduser().absolute()
    out = a.out.expanduser().absolute() if a.out else game.parent
    logs = work / 'logs'
    check_setup(work, out, (game, update, keys))
    t0 = time.time()

    py = environment(work, logs)
    version = code(py, update, keys, work, logs)
    data(py, game, update, keys, work, version, logs)
    rewrite(py, work, version, logs)

    make = ['make', f'WORK={work}', f'VERSION={version}', f'PY={py}']
    say('4/5 runtime: building the runtime and the shader tools')
    run([*make, 'prepare'], logs / 'prepare.log')
    run([*make, '-j8', 'runtime', 'shader-tools'], logs / 'runtime.log')

    app = out / f'{APP_NAME}.app'
    say(f'5/5 app: assembling {app}')
    try:
        run([*make, 'app', f'APP={app}', f'GAME={update}', f'KEYS={keys}'], logs / 'app.log')
    except BuildError as error:
        shutil.rmtree(app, ignore_errors=True)
        raise BuildError(f'{error}\nthe unfinished app was removed') from None
    say(f'done in {elapsed(time.time() - t0)}: {app} ({versions.expected(version)["display_version"]})')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except BuildError as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('\nstopped; run the same command again to continue from the last finished step', file=sys.stderr)
        sys.exit(130)
