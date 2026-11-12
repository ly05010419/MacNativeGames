"""The cache folder, its lock, downloads and the staging every recipe uses. Python 3.9 safe: a Makefile may run this
with macOS's /usr/bin/python3."""
import fcntl
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import threading
from contextlib import contextmanager
from pathlib import Path


class DepsError(RuntimeError):
    pass


def cache():
    """~/Library/Caches/MacNativeGames/deps, or MNG_DEPS_CACHE: what is here can be deleted and is made again."""
    return Path(os.environ.get('MNG_DEPS_CACHE') or Path.home() / 'Library/Caches/MacNativeGames/deps')


def say(text):
    print(f'[deps] {text}', file=sys.stderr, flush=True)


_guard = threading.RLock()
_depth = 0


@contextmanager
def locked():
    """The cache's lock (cache()/.lock), held once per process however deep the calls nest: a second flock on a new
    open() of the same file in the same process would wait for itself."""
    global _depth
    with _guard:
        if _depth:
            _depth += 1
            try:
                yield
            finally:
                _depth -= 1
            return
        folder = cache()
        folder.mkdir(parents=True, exist_ok=True)
        with open(folder / '.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)     # another build may be making the same thing
            _depth = 1
            try:
                yield
            finally:
                _depth = 0


def logs():
    folder = cache() / 'logs'
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def run(args, log_name, cwd=None, env=None):
    log = logs() / log_name
    with open(log, 'w') as f:
        done = subprocess.run([str(a) for a in args], cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if done.returncode:
        tail = log.read_text(errors='replace').splitlines()[-15:]
        raise DepsError(f'{args[0]} failed (exit {done.returncode}); log {log}:\n' + '\n'.join(tail))


def digest(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(url, want, name):
    """cache()/src/<name>, downloaded once and kept only when its digest is want (SHA-256, or SHA-512 by length). With
    macOS's curl, not urllib: python.org's Python has no CA certificates until its "Install Certificates.command"."""
    algorithm = 'sha512' if len(want) == 128 else 'sha256'
    archive = cache() / 'src' / name
    if archive.is_file() and digest(archive, algorithm) == want:
        return archive
    archive.parent.mkdir(parents=True, exist_ok=True)
    partial = archive.with_name(archive.name + '.partial')
    # downloads.xiph.org sometimes drops the TLS handshake (curl exit 35), which --retry alone does not retry
    run(['/usr/bin/curl', '-fL', '--retry', '5', '--retry-delay', '2', '--retry-all-errors', '-o', partial, url],
        f'download-{name}.log')
    if digest(partial, algorithm) != want:
        partial.unlink()
        raise DepsError(f'{url} does not match its pinned {algorithm.upper()}')
    partial.replace(archive)
    return archive


def unpack(archive, folder):
    """A fresh folder with the archive's contents (tar or zip)."""
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    if archive.name.endswith('.zip'):
        run(['/usr/bin/ditto', '-x', '-k', archive, folder], f'unpack-{archive.name}.log')
        return folder
    with tarfile.open(archive) as t:
        if hasattr(tarfile, 'data_filter'):
            t.extractall(folder, filter='data')
        else:                                      # Python 3.9 without the 3.12 filters: refuse paths leaving the folder
            for member in t.getmembers():
                target = (folder / member.name).resolve()
                if folder.resolve() not in target.parents and target != folder.resolve():
                    raise DepsError(f'{archive}: {member.name} points outside the folder')
                if member.issym() or member.islnk():
                    link = (target.parent / member.linkname).resolve()
                    if folder.resolve() not in link.parents:
                        raise DepsError(f'{archive}: {member.name} links outside the folder')
            t.extractall(folder)
    return folder


def git_fetch(url, commit, folder):
    """folder checked out at commit (a shallow fetch of that commit only)."""
    head = subprocess.run(['git', '-C', folder, 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if folder.is_dir() and head.returncode == 0 and head.stdout.strip() == commit:
        return folder
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    name = folder.name
    run(['git', 'init', '-q', folder], f'{name}-git-init.log')
    run(['git', '-C', folder, 'fetch', '-q', '--depth', '1', url, commit], f'{name}-git-fetch.log')
    run(['git', '-C', folder, 'checkout', '-q', 'FETCH_HEAD'], f'{name}-git-checkout.log')
    return folder


def install(prefix, steps):
    """steps(stage) installs into a staging folder, which becomes prefix only when it finished."""
    stage = prefix.with_name(prefix.name + '.partial')
    if stage.exists():
        shutil.rmtree(stage)
    steps(stage)
    if prefix.exists():
        shutil.rmtree(prefix)
    stage.rename(prefix)


def jobs():
    return str(os.cpu_count() or 4)
