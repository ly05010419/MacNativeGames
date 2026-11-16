#!/usr/bin/env python3
"""Run a Release app's job with its bundled source snapshot, without a developer checkout.

The signed app stays read-only. Each source archive has its own persistent directory under Application Support;
old versions stay available because built games record their source folder for later updates.
From 1.0.2 a job may run newer code instead: a signed source release that source_updates.py (beside this file in the
signed app) verified and installed; see docs/builder-app.md "Code updates without a new Launcher".
"""
import fcntl
import hashlib
import os
from pathlib import Path
import plistlib
import re
import runpy
import shutil
import subprocess
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


def updates_conf(resources):
    """source_updates (beside this file in the signed app) and its settings, or (None, None) for a Launcher without a
    source key or without the cryptography module: it then runs its own source, as before."""
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import source_updates
        conf = source_updates.settings(resources)
    except Exception as error:   # an unreadable Info.plist or a missing module must not stop the Launcher's jobs
        print(f'code updates unavailable: {error}', file=sys.stderr)
        return None, None
    if not conf or os.environ.get('MNG_SOURCE_UPDATES', '1') == '0':
        return None, None
    return source_updates, conf


def fetch_in_background(support):
    """Starts `bundled_job.py --fetch-source` on its own: the home page's job returns at once."""
    root = support / 'sources'
    root.mkdir(parents=True, exist_ok=True)
    with (root / 'update-download.log').open('a') as log:
        subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), '--fetch-source'], stdin=subprocess.DEVNULL,
                         stdout=log, stderr=log, start_new_session=True, close_fds=True, cwd=str(root))


def refresh(su, conf, support, job):
    """Newer signed code before a job. An update ('build') downloads it first and says so in its log; the home page
    ('apps') reads the release list at most once in ten minutes and downloads in the background; other jobs only use
    what is here. Failures leave the code already on this Mac."""
    if job == 'build':
        say = lambda line: print(line, flush=True)   # a build's plain lines show in the Launcher's log
        try:
            kind, found = su.check(support, conf)
            if kind == 'new':
                su.download(support, found, conf, say)
            elif kind == 'launcher':
                say(su.text(f'Code update: newer code needs MNG Launcher {found["min_launcher"]} or newer; download it from '
                            f'the Releases page. This update uses the code already here.',
                            f'代码更新：更新的代码需要 MNG Launcher {found["min_launcher"]} 或更新版本，请到 Releases 页面下载。'
                            f'这次更新用本机已有的代码。'))
        except (su.SourceError, OSError, tarfile.TarError) as error:
            say(su.text(f'Code update: could not get newer code ({error}); this update uses the code already here.',
                        f'代码更新：没能取到新代码（{error}），这次更新用本机已有的代码。'))
    elif job == 'apps' and not su.checked_recently(support):
        try:
            kind, _ = su.check(support, conf)
        except (su.SourceError, OSError) as error:
            print(f'code updates: {error}', file=sys.stderr)
            return
        if kind == 'new':
            fetch_in_background(support)


def main():
    resources = Path(__file__).resolve().parent
    support = Path.home() / 'Library/Application Support/MacNativeGames/Builder'
    bundled = prepare(resources, support).resolve()
    su, conf = updates_conf(resources)
    job = sys.argv[1] if len(sys.argv) > 1 else ''
    if job == '--fetch-source':
        found = su.pending(support) if su else None
        if found:
            su.download(support, found, conf, lambda line: print(line, flush=True))
        return
    source = bundled
    if su:
        refresh(su, conf, support, job)
        source = su.choose(support, bundled, conf).resolve()
        if job == 'build':
            commit = (source / '.mng-source-commit').read_text().strip()[:12]
            print(su.text(f'Code: {commit} ({"a signed code update" if source != bundled else "the code this Launcher carries"})',
                          f'代码：{commit}（{"签名的代码更新" if source != bundled else "Launcher 自带的代码"}）'), flush=True)
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
