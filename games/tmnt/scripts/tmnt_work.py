"""Verified, bounded build steps in a fixed private work directory.

Receipts are caches, not provenance: inputs and outputs are rehashed on reuse.
Only explicitly owned outputs are replaced, never the work root or user saves.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import importlib.metadata
import contextlib
import fcntl


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(path, ignore=()):
    path = Path(path)
    if path.is_symlink():
        raise ValueError(f'cache path is a symlink: {path}')
    if path.is_file():
        return {'kind': 'file', 'sha256': digest(path), 'mode': path.stat().st_mode & 0o777}
    if not path.is_dir():
        raise ValueError(f'cache path missing: {path}')
    files = {}
    for item in sorted(path.rglob('*')):
        relative = item.relative_to(path)
        if any(part in ignore for part in relative.parts):
            continue
        if item.is_symlink():
            raise ValueError(f'cache entry is a symlink: {item}')
        if item.is_file():
            files[str(relative)] = {'sha256': digest(item), 'mode': item.stat().st_mode & 0o777}
    return {'kind': 'directory', 'files': files}


def source_identity(game):
    """Conservative invalidation for preparation scripts and pinned rewriters."""
    result = {'python': sys.version.split()[0]}
    for package in ('capstone', 'lz4'):
        result[package] = importlib.metadata.version(package)
    for relative in ('scripts', 'assets', 'third_party/d2r', 'tools/totk_xlate',
                     'third_party/ryujinx'):
        result[relative] = inventory(game / relative, ('__pycache__', 'obj', 'bin'))
    result['dependency-pins'] = inventory(game.parents[1] / 'tools/deps/pins.py')
    return result


def protect_inputs(inputs, outputs):
    """Reject overlapping user inputs before any managed output is modified."""
    for source in inputs:
        if source is None:
            continue
        source = Path(source).resolve()
        for target in outputs:
            target = Path(target).resolve()
            if source == target or source.is_relative_to(target) or target.is_relative_to(source):
                raise ValueError(f'input and managed output must be separate: {source}, {target}')


class Steps:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.receipts = self.root / '.verified-steps'
        if self.receipts.is_symlink():
            raise ValueError('receipt directory must not be a symlink')
        self.receipts.mkdir(parents=True, exist_ok=True)

    def run(self, name, inputs, outputs, action, ignore=()):
        if not name.replace('-', '').isalnum():
            raise ValueError('invalid step name')
        outputs = [Path(p) for p in outputs]
        for path in outputs:
            if path.is_symlink() or not path.resolve().is_relative_to(self.root) or path.resolve() == self.root:
                raise ValueError(f'unsafe cache output: {path}')
        receipt = self.receipts / (name + '.json')
        identity = json.loads(json.dumps(inputs, sort_keys=True))
        reason = 'no valid receipt'
        try:
            old = json.loads(receipt.read_text())
            if not isinstance(old, dict):
                raise ValueError('invalid receipt object')
            if old.get('inputs') != identity:
                reason = 'inputs or tools changed'
            elif old.get('outputs') != [inventory(p, ignore) for p in outputs]:
                reason = 'output bytes changed'
            else:
                print(f'[tmnt] reuse {name}: inputs and outputs verified', flush=True)
                return False
        except (OSError, ValueError, KeyError):
            pass
        print(f'[tmnt] rebuild {name}: {reason}', flush=True)
        # Invalidate before touching outputs; failed steps cannot be reused.
        receipt.unlink(missing_ok=True)
        for path in outputs:
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
        action()
        result = {'inputs': identity, 'outputs': [inventory(p, ignore) for p in outputs]}
        temporary = receipt.with_suffix('.tmp')
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(json.dumps(result, sort_keys=True) + '\n')
        os.replace(temporary, receipt)
        return True


def adopt_romfs(app, output):
    """Adopt only an exact pinned RomFS, rebuilding its index from actual bytes."""
    from expected_inputs import check_supported_romfs
    from extract_romfs import index_romfs
    source = Path(app) / 'Contents/Resources/game/romfs/romfs.bin'
    try:
        sha = digest(source)
        check_supported_romfs(sha)
    except (OSError, ValueError):
        print('[tmnt] extract RomFS: installed App has no verified supported image', flush=True)
        return False
    files = index_romfs(source)
    output.mkdir(parents=True)
    shutil.copy2(source, output / 'romfs.bin')
    if digest(output / 'romfs.bin') != sha:
        raise ValueError('installed RomFS changed during adoption')
    (output / 'index.json').write_text(json.dumps({
        'status': 'pass', 'romfs_sha256': sha, 'romfs_size': source.stat().st_size,
        'files': files, 'integrity': 'exact pinned RomFS SHA-256 verified from installed App',
    }, sort_keys=True) + '\n')
    print('[tmnt] reuse installed App RomFS: exact supported digest verified', flush=True)
    return True


@contextlib.contextmanager
def build_lock(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / '.tmnt-build.lock'
    # O_NOFOLLOW prevents a stale/private work-tree link from redirecting writes.
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(f'another TMNT build is using {root}') from None
        yield
