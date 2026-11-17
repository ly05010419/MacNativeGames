#!/usr/bin/env python3
"""Make "MNG Launcher.app": the SwiftUI window of tools/builder_app, with a Python of its own inside, so a
player needs only the developer tools (xcode-select --install) and this checkout.

  python3 tools/builder_app/make_app.py [-o "~/Applications/MNG Launcher.app"] [--no-python]

Steps: swift build (release); the pinned Python of python-build-standalone (PSF licence), downloaded once into the
cache and checked against its SHA-256; the one package the window's job layer needs before a build (cryptography, to
read a game card with the player's keys, at games/totk/requirements.txt's version); Info.plist with this checkout's path
(MNGRepo), so the window runs its tools/builder_jobs; an ad-hoc signature. Runs with the system's python3 (3.9)."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
NAME = 'MNG Launcher'   # the app's name (user 2026-11-12: "这个打包工具叫 MNG Launcher")
BUNDLE_ID = 'org.macnativegames.launcher'
EXECUTABLE = 'MNGLauncher'   # Contents/MacOS (swift build names its product MNGBuilder, after the target)
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


def zip_app(app, archive):
    # Keep the public local-build path independent of the maintainer's signing
    # tools. Omit xattrs so Archive Utility cannot add AppleDouble sidecars.
    run(['/usr/bin/ditto', '-c', '-k', '--norsrc', '--noextattr', '--keepParent', app, archive])
    archive.with_name(archive.name + '.sha256').write_text(sha256(archive) + '  ' + archive.name + '\n')


def python_archive(cache):
    archive = cache / PYTHON_URL.rsplit('/', 1)[1].replace('%2B', '+')
    if archive.is_file() and sha256(archive) == PYTHON_SHA256:
        return archive
    cache.mkdir(parents=True, exist_ok=True)
    partial = archive.with_name(archive.name + '.partial')
    say(f'downloading {PYTHON_URL}')
    # macOS's curl, not urllib: python.org's Python has no CA certificates until its Install Certificates step
    subprocess.run(['/usr/bin/curl', '-fL', '--retry', '3', '-o', partial, PYTHON_URL], check=True)
    got = sha256(partial)
    if got != PYTHON_SHA256:
        partial.unlink()
        raise SystemExit(f'the Python archive does not match its pinned SHA-256 ({got})')
    partial.replace(archive)
    return archive


def binary(scratch):
    env = dict(os.environ)
    # --disable-sandbox: SwiftPM compiles a dependency's manifest (SwiftTerm, pinned in Package.swift) in a sandbox of
    # its own, which cannot be made inside another one (this Mac's storage_guard runs every build in sandbox-exec)
    run(['swift', 'build', '-c', 'release', '--disable-sandbox', '--package-path', HERE, '--scratch-path', scratch], env=env)
    out = subprocess.run(['swift', 'build', '-c', 'release', '--disable-sandbox', '--package-path', str(HERE),
                          '--scratch-path', str(scratch), '--show-bin-path'], check=True, capture_output=True, text=True).stdout.strip()
    return Path(out) / 'MNGBuilder'


def licenses(scratch, resources):
    """The licence and notice files of every package SwiftPM checked out (SwiftTerm, MIT; swift-argument-parser,
    Apache-2.0, which only SwiftTerm's own tools use), into Contents/Resources/licenses/<package>/: the app carries
    their code. Returns the packages."""
    done = []
    for package in sorted(p for p in (Path(scratch) / 'checkouts').glob('*') if p.is_dir()):
        files = [f for f in package.iterdir() if f.is_file() and re.match(r'(LICEN[CS]E|NOTICE|COPYING)', f.name, re.I)]
        if not files:
            raise SystemExit(f'{package.name}: no licence file in its checkout; the app cannot carry its code without one')
        target = resources / 'licenses' / package.name
        target.mkdir(parents=True, exist_ok=True)
        for f in files:
            shutil.copy2(f, target / f.name)
        done.append(package.name)
    return done


SOURCE_REPO = 'ly05010419/MacNativeGames'   # where signed source releases (tag source-*) are published
SOURCE_KEY = HERE / 'source_signing.pub'      # their Ed25519 public key; the private key stays in the release tool's keychain


def check_release_source(repo, commit='HEAD'):
    """Only a committed tree is distributable: no ignored notes, keys, build data or unrelated untracked files.
    Archive the source as data so signing the app cannot change the pinned translator executables inside it.
    commit: HEAD for a Launcher (its window and source must match the checkout), or the commit of a source release.
    """
    if commit == 'HEAD' and subprocess.run(['git', '-C', repo, 'diff', '--quiet', 'HEAD', '--']).returncode:
        raise ValueError('Portable builds require committed source changes; the window and bundled source must match.')
    names = set(subprocess.check_output(['git', '-C', repo, 'ls-tree', '-r', '--name-only', commit], text=True).splitlines())
    required = {
        'build_app.py', 'tools/builder_jobs/__main__.py', 'tools/builder_app/bundled_job.py',
        'tools/builder_app/source_updates.py', 'tools/builder_app/source_signing.pub',
        'games/totk/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h',
        'games/oot/ios/tools/seed_image.py', 'games/totk/ios/tools/seed_image.py',
    }
    missing = required - names
    if missing:
        raise ValueError('Release source is incomplete; commit these dependencies first: ' + ', '.join(sorted(missing)))
    forbidden = names & {'CLAUDE.md', 'AGENTS.md', 'prod.keys', 'title.keys'}
    if forbidden:
        raise ValueError('Local-only files in release source: ' + ', '.join(sorted(forbidden)))


def build_python():
    """The Python 3.14 the prebuilt programs are made with (build_app.py reads zstd with the standard library)."""
    for name in ('python3.14', 'python3.15'):
        found = shutil.which(name, path=os.pathsep.join(['/usr/local/bin', '/opt/homebrew/bin', '/Library/Frameworks/Python.framework/Versions/Current/bin']))
        if found:
            return found
    raise SystemExit('the prebuilt programs need Python 3.14 (python.org)')


def prebuilt_programs(repo, commit, scratch):
    """tools/prebuilt.py build in an unpacked copy of the commit, in scratch (outside the checkout): the folder of
    programs a source archive carries so a player's build compiles nothing (user 2026-11-17). Reused when scratch
    already holds this commit's complete folder."""
    full = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', '--verify', commit + '^{commit}'], text=True).strip()
    if subprocess.run(['git', '-C', str(repo), 'cat-file', '-e', f'{full}:tools/prebuilt.py'], capture_output=True).returncode:
        return None   # a commit before tools/prebuilt.py: its archive is the source alone, as it was
    copy = Path(scratch) / f'prebuilt-{full[:12]}'
    sys.path.insert(0, str(repo))
    try:
        import tools.prebuilt as prebuilt
    finally:
        sys.path.remove(str(repo))
    if copy.is_dir() and prebuilt.complete(copy):
        return copy / prebuilt.FOLDER
    shutil.rmtree(copy, ignore_errors=True)
    copy.mkdir(parents=True)
    archive = subprocess.run(['git', '-C', str(repo), 'archive', '--format=tar', full], check=True, capture_output=True).stdout
    import io
    with tarfile.open(fileobj=io.BytesIO(archive)) as t:
        t.extractall(copy, filter='data') if sys.version_info >= (3, 12) else t.extractall(copy)
    (copy / '.mng-source-commit').write_text(full + '\n')
    say(f'prebuilt programs of {full[:12]} (tools/prebuilt.py, a few minutes)')
    run([build_python(), '-B', copy / 'tools/prebuilt.py', 'build', copy])
    if not prebuilt.complete(copy):
        raise SystemExit(f'{copy}: tools/prebuilt.py did not make every program')
    return copy / prebuilt.FOLDER


def source_archive(repo, destination, commit='HEAD', prebuilt=None):
    """git archive of the commit (tar.gz), with prebuilt (a .prebuilt folder of this commit, prebuilt_programs) added
    as .prebuilt when given."""
    check_release_source(repo, commit)
    if prebuilt is None:
        run(['git', '-C', repo, 'archive', '--format=tar.gz', '-o', destination, commit])
        return sha256(destination)
    import gzip
    tar = Path(str(destination) + '.tar')
    run(['git', '-C', repo, 'archive', '--format=tar', '-o', tar, commit])

    def plain(info):   # no owner or build machine names in the archive
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        return info
    with tarfile.open(tar, 'a') as t:
        t.add(prebuilt, arcname='.prebuilt', filter=plain)
    with open(tar, 'rb') as src, gzip.open(destination, 'wb') as dst:
        shutil.copyfileobj(src, dst)
    tar.unlink()
    return sha256(destination)


def source_settings(repo, commit='HEAD'):
    """What a Release Launcher needs for signed code updates (tools/builder_app/source_updates.py): the public key, the
    repository its source releases are on, and its own source's commit time (a release is newer only by that)."""
    key = SOURCE_KEY.read_text().strip()
    return {
        'MNGSourcePublicKey': key, 'MNGSourceRepo': SOURCE_REPO,
        'MNGSourceCommitTime': int(subprocess.check_output(['git', '-C', str(repo), 'show', '-s', '--format=%ct', commit],
                                                           text=True).strip()),
    }


def icon(resources):
    """Resources/AppIcon.icns from tools/builder_app/Resources/AppIcon.png (the project's own artwork, 1024 px, the shape
    on the macOS icon grid), every size iconutil wants, drawn with AppKit (resize_icon.js; sips writes through the
    user's temporary folder, which this Mac's build sandbox refuses)."""
    art = HERE / 'Resources/AppIcon.png'
    iconset = resources / 'AppIcon.iconset'
    iconset.mkdir()
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
            run(['/usr/bin/osascript', '-l', 'JavaScript', HERE / 'resize_icon.js', art, str(size * scale), iconset / name],
                stdout=subprocess.DEVNULL)
    run(['iconutil', '-c', 'icns', iconset, '-o', resources / 'AppIcon.icns'])
    shutil.rmtree(iconset)


def info(repo):
    commit = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    return {
        'CFBundleName': NAME, 'CFBundleDisplayName': NAME, 'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleExecutable': EXECUTABLE, 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1', 'LSMinimumSystemVersion': '26.0', 'NSHighResolutionCapable': True,
        'LSApplicationCategoryType': 'public.app-category.developer-tools',
        'MNGRepo': str(repo), 'MNGSourceCommit': commit, 'CFBundleIconFile': 'AppIcon',
    }


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--out', type=Path, default=Path.home() / 'Applications' / f'{NAME}.app')
    ap.add_argument('--scratch', type=Path, default=CACHE / 'swift', help='the Swift build folder')
    ap.add_argument('--no-python', action='store_true', help='use /opt/homebrew/bin/python3.14 instead of carrying one')
    ap.add_argument('--portable', action='store_true', help='bundle committed source for download and installation on another Mac')
    # No default (user 2026-11-17: "版本号 不对？？？"): a default of 1.0.0 made every Launcher built from a checkout show
    # "1.0.0", a release that is not it. A release (--portable) names its version; one built here has none and says so
    # (App.swift VersionLabel: 开发版 and its commit).
    ap.add_argument('--version', help='release version (major.minor.patch); --portable needs it')
    ap.add_argument('--zip', type=Path, help='also write a ZIP download containing the app')
    a = ap.parse_args(argv)
    out = a.out.expanduser().resolve()
    if out.suffix != '.app':
        raise SystemExit('-o must end in .app')
    if a.portable and a.version is None:
        raise SystemExit('--portable needs --version (major.minor.patch)')
    if a.version is not None and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', a.version):
        raise SystemExit('--version must be major.minor.patch')
    if a.portable and (a.no_python or platform.machine() != 'arm64'):
        raise SystemExit('Portable releases require Apple silicon and the bundled Python (no --no-python).')
    if out == REPO or REPO in out.parents:
        raise SystemExit('The built app must be outside the source checkout.')
    if a.portable:
        check_release_source(REPO)
    exe = binary(a.scratch.expanduser())
    stage = out.with_name(out.name + '.partial')
    shutil.rmtree(stage, ignore_errors=True)
    (stage / 'Contents/MacOS').mkdir(parents=True)
    (stage / 'Contents/Resources').mkdir()
    shutil.copy2(exe, stage / 'Contents/MacOS' / EXECUTABLE)
    say('licences: ' + ', '.join(licenses(a.scratch.expanduser(), stage / 'Contents/Resources')))
    icon(stage / 'Contents/Resources')
    plist = info(REPO)
    if a.version is not None:   # a checkout's build keeps info()'s placeholder; its window says 开发版, not a version
        plist['CFBundleShortVersionString'] = a.version
        plist['CFBundleVersion'] = a.version
    if a.portable:
        plist.pop('MNGRepo')
        plist['MNGSourceCommit'] = subprocess.check_output(['git', '-C', REPO, 'rev-parse', 'HEAD'], text=True).strip()
        # with the programs a build compiles, made from this commit (tools/prebuilt.py): players need no command line tools
        programs = prebuilt_programs(REPO, 'HEAD', a.scratch.expanduser().parent / 'prebuilt')
        plist['MNGSourceArchiveSHA256'] = source_archive(REPO, stage / 'Contents/Resources/source.tar.gz', prebuilt=programs)
        plist.update(source_settings(REPO))
        # the code that trusts or refuses downloaded source lives in the signed app, never in a source archive
        for name in ('bundled_job.py', 'source_updates.py'):
            shutil.copy2(HERE / name, stage / 'Contents/Resources' / name)
    if not a.no_python:
        with tarfile.open(python_archive(CACHE)) as t:
            t.extractall(stage / 'Contents/Resources', filter='data') if sys.version_info >= (3, 12) else t.extractall(stage / 'Contents/Resources')
        py = stage / 'Contents/Resources/python/bin/python3.14'
        run([py, '-m', 'pip', 'install', '-q', '--disable-pip-version-check', '--no-compile', requirement('cryptography')])
    else:
        plist['MNGPython'] = '/opt/homebrew/bin/python3.14'
    with open(stage / 'Contents/Info.plist', 'wb') as f:
        plistlib.dump(plist, f)
    # The designated requirement is the bundle ID, not the ad-hoc default (this build's code hash): macOS keeps the
    # "removable volume" permission by it, so a rebuilt Launcher is not asked again (2026-11-15: TCC.db held a cdhash for
    # org.macnativegames.launcher; build_app.sign_app does the same for the game apps).
    run(['codesign', '--force', '--deep', '--sign', '-', f'-r=designated => identifier "{BUNDLE_ID}"', stage])
    run(['codesign', '--verify', '--deep', '--strict', stage])
    if out.exists():
        shutil.rmtree(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    stage.rename(out)
    say(f'made {out} (source {REPO}, commit {plist["MNGSourceCommit"]}); open it from Finder')
    if a.zip:
        archive = a.zip.expanduser().resolve()
        if archive == REPO or REPO in archive.parents or archive == out or out in archive.parents:
            raise SystemExit('The ZIP must be outside the source checkout and the app.')
        archive.parent.mkdir(parents=True, exist_ok=True)
        zip_app(out, archive)
        say(f'download: {archive} (ad-hoc signed; not notarized)')


if __name__ == '__main__':
    main(sys.argv[1:])
