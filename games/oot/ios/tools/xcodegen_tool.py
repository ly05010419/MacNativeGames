"""XcodeGen at a pinned release, downloaded once per Mac, so an iPhone / iPad build needs no Homebrew.

The official release zip (a universal binary with its share/xcodegen presets) is checked against its SHA-256 (the
digest GitHub lists for the asset) and unpacked into ~/Library/Caches/MacNativeGames/deps/xcodegen-<version>
(MNG_DEPS_CACHE moves it), where build_app.py keeps the other pinned build dependencies. macOS's curl downloads
it: python.org's Python has no CA certificates until its Install Certificates step. Homebrew's xcodegen is not
used, so every Mac generates the same project.
"""
import fcntl
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

VERSION = '2.46.0'
URL = f'https://github.com/yonaskolb/XcodeGen/releases/download/{VERSION}/xcodegen.zip'
SHA256 = '4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806'


def deps():
    return Path(os.environ.get('MNG_DEPS_CACHE') or Path.home() / 'Library/Caches/MacNativeGames/deps')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def xcodegen():
    """The path of the pinned xcodegen, downloading and unpacking it the first time."""
    home = deps() / f'xcodegen-{VERSION}'
    program = home / 'bin/xcodegen'
    if program.is_file():
        return str(program)
    home.parent.mkdir(parents=True, exist_ok=True)
    with open(home.parent / '.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)       # another build may be fetching it at the same time
        if program.is_file():
            return str(program)
        archive = home.parent / f'src/xcodegen-{VERSION}.zip'
        archive.parent.mkdir(parents=True, exist_ok=True)
        if not archive.is_file() or sha256(archive) != SHA256:
            partial = archive.with_name(archive.name + '.partial')
            subprocess.run(['/usr/bin/curl', '-fsSL', '--retry', '2', '-o', partial, URL], check=True)
            if sha256(partial) != SHA256:
                partial.unlink()
                raise RuntimeError(f'XcodeGen {VERSION} from {URL} does not match its pinned SHA-256')
            partial.replace(archive)
        stage = home.with_name(home.name + '.partial')
        shutil.rmtree(stage, ignore_errors=True)
        subprocess.run(['/usr/bin/ditto', '-x', '-k', archive, stage], check=True)
        unpacked = stage / 'xcodegen'                # the zip holds xcodegen/{bin,share,LICENSE}
        if not (unpacked / 'bin/xcodegen').is_file():
            raise RuntimeError(f'{archive}: no xcodegen/bin/xcodegen inside')
        unpacked.rename(home)
        shutil.rmtree(stage, ignore_errors=True)
    return str(program)
