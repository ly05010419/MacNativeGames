#!/usr/bin/env python3
"""Run a Release app's job with its bundled source snapshot, without a developer checkout.

The signed app stays read-only. Each source archive has its own persistent directory under Application Support;
old versions stay available because built games record their source folder for later updates.
"""
import fcntl
import hashlib
import os
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import sys
import tarfile
import tempfile


def prepare(resources, support):
    resources, support = Path(resources), Path(support)
    info = plistlib.loads((resources.parent / 'Info.plist').read_bytes())
    digest = info.get('MNGSourceArchiveSHA256', '')
    if not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError('The Launcher source archive has no valid SHA-256.')
    root = support / 'sources'
    root.mkdir(parents=True, exist_ok=True)
    target = root / digest
    # Multiple jobs can start together (checks, installed apps, page queries).
    with (root / '.install.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if (target / '.ready').is_file() and (target / 'tools/builder_jobs/__main__.py').is_file():
            return target
        archive = resources / 'source.tar.gz'
        with archive.open('rb') as f:
            actual = hashlib.file_digest(f, 'sha256').hexdigest()
        if actual != digest:
            raise ValueError('The Launcher source archive is damaged. Download the app again.')
        stage = Path(tempfile.mkdtemp(prefix='.source-', dir=root))
        try:
            with tarfile.open(archive) as tar:
                tar.extractall(stage, filter='data')
            if not (stage / 'tools/builder_jobs/__main__.py').is_file() or not (stage / 'build_app.py').is_file():
                raise ValueError('The Launcher source archive is incomplete.')
            (stage / '.mng-source-commit').write_text(info['MNGSourceCommit'] + '\n')
            (stage / '.ready').write_text(digest + '\n')
            if target.exists():
                raise ValueError(f'Incomplete source folder: {target}. Move it aside and reopen the Launcher.')
            stage.rename(target)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    return target


def main():
    resources = Path(__file__).resolve().parent
    support = Path.home() / 'Library/Application Support/MacNativeGames/Builder'
    source = prepare(resources, support).resolve()
    os.chdir(source)
    sys.path.insert(0, str(source))
    os.environ['PYTHONPATH'] = str(source)
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    os.environ['MNG_REPO'] = str(source)
    os.environ['MNG_LAUNCHER_APP'] = str(resources.parent.parent)
    runpy.run_module('tools.builder_jobs', run_name='__main__', alter_sys=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as error:
        sys.exit(str(error))
