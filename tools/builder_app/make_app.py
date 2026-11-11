#!/usr/bin/env python3
"""Make "MacNativeGames Builder.app": the SwiftUI window of tools/builder_app, with a Python of its own inside, so a
player needs only the developer tools (xcode-select --install) and this checkout.

  python3 tools/builder_app/make_app.py [-o "~/Applications/MacNativeGames Builder.app"] [--no-python]

Steps: swift build (release); the pinned Python of python-build-standalone (PSF licence), downloaded once into the
cache and checked against its SHA-256; the one package the window's job layer needs before a build (cryptography, to
read a game card with the player's keys, at games/totk/requirements.txt's version); Info.plist with this checkout's path
(MNGRepo), so the window runs its tools/builder_jobs; an ad-hoc signature. Runs with the system's python3 (3.9)."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
NAME = 'MacNativeGames Builder'
BUNDLE_ID = 'org.macnativegames.builder'
# python-build-standalone release 20261021 (three weeks old when pinned), CPython 3.14.9; digest from the release asset
PYTHON_URL = ('https://github.com/astral-sh/python-build-standalone/releases/download/20261021/'
              'cpython-3.14.9%2B20261021-aarch64-apple-darwin-install_only.tar.gz')
PYTHON_SHA256 = '226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a953c9f507755e36a'
CACHE = Path.home() / 'Library/Caches/MacNativeGames/builder-app'


def say(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    say('$ ' + ' '.join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def requirement(name):
    """The pinned version of a package in games/totk/requirements.txt, the build's own pins."""
    for line in (REPO / 'games/totk/requirements.txt').read_text().splitlines():
        m = re.match(r'\s*' + re.escape(name) + r'==([^\s#]+)', line)
        if m:
            return f'{name}=={m.group(1)}'
    raise SystemExit(f'{name} is not pinned in games/totk/requirements.txt')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def python_archive(cache):
    archive = cache / PYTHON_URL.rsplit('/', 1)[1].replace('%2B', '+')
    if archive.is_file() and sha256(archive) == PYTHON_SHA256:
        return archive
    cache.mkdir(parents=True, exist_ok=True)
    partial = archive.with_name(archive.name + '.partial')
    say(f'downloading {PYTHON_URL}')
    with urllib.request.urlopen(PYTHON_URL) as r, open(partial, 'wb') as f:
        shutil.copyfileobj(r, f)
    got = sha256(partial)
    if got != PYTHON_SHA256:
        partial.unlink()
        raise SystemExit(f'the Python archive does not match its pinned SHA-256 ({got})')
    partial.replace(archive)
    return archive


def binary(scratch):
    env = dict(os.environ)
    run(['swift', 'build', '-c', 'release', '--package-path', HERE, '--scratch-path', scratch], env=env)
    out = subprocess.run(['swift', 'build', '-c', 'release', '--package-path', str(HERE), '--scratch-path', str(scratch),
                          '--show-bin-path'], check=True, capture_output=True, text=True).stdout.strip()
    return Path(out) / 'MNGBuilder'


def info(repo):
    commit = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return {
        'CFBundleName': NAME, 'CFBundleDisplayName': NAME, 'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleExecutable': 'MNGBuilder', 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1', 'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
        'LSApplicationCategoryType': 'public.app-category.developer-tools',
        'MNGRepo': str(repo), 'MNGSourceCommit': commit,
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--out', type=Path, default=Path.home() / 'Applications' / f'{NAME}.app')
    ap.add_argument('--scratch', type=Path, default=CACHE / 'swift', help='the Swift build folder')
    ap.add_argument('--no-python', action='store_true', help='use /opt/homebrew/bin/python3.14 instead of carrying one')
    a = ap.parse_args(argv)
    out = a.out.expanduser().resolve()
    if out.suffix != '.app':
        raise SystemExit('-o must end in .app')
    exe = binary(a.scratch.expanduser())
    stage = out.with_name(out.name + '.partial')
    shutil.rmtree(stage, ignore_errors=True)
    (stage / 'Contents/MacOS').mkdir(parents=True)
    (stage / 'Contents/Resources').mkdir()
    shutil.copy2(exe, stage / 'Contents/MacOS/MNGBuilder')
    plist = info(REPO)
    if not a.no_python:
        with tarfile.open(python_archive(CACHE)) as t:
            t.extractall(stage / 'Contents/Resources', filter='data') if sys.version_info >= (3, 12) else t.extractall(stage / 'Contents/Resources')
        py = stage / 'Contents/Resources/python/bin/python3.14'
        run([py, '-m', 'pip', 'install', '-q', '--disable-pip-version-check', '--no-compile', requirement('cryptography')])
    else:
        plist['MNGPython'] = '/opt/homebrew/bin/python3.14'
    with open(stage / 'Contents/Info.plist', 'wb') as f:
        plistlib.dump(plist, f)
    run(['codesign', '--force', '--deep', '--sign', '-', stage])
    run(['codesign', '--verify', '--deep', '--strict', stage])
    if out.exists():
        shutil.rmtree(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    stage.rename(out)
    say(f'made {out} (source {REPO}, commit {plist["MNGSourceCommit"]}); open it from Finder')


if __name__ == '__main__':
    main(sys.argv[1:])
