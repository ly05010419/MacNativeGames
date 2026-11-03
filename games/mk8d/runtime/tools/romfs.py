"""Read-only paths inside DMROMFS1 images, without extracting their contents.

Readers opt into Path/open/os/mmap here; the real filesystem is untouched. A path
like /game/RomFS.img/Data/archive is a bounded view of that image entry. Ordinary
paths retain their normal behavior, including writes to generated outputs.
"""
import builtins
import errno
import fnmatch
import functools
import io
import mmap as _mmap
import os as _os
from pathlib import Path as _Path
import stat as _stat
import struct
import sys
from types import SimpleNamespace
from typing import TYPE_CHECKING

HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
MAGIC = b'DMROMFS1'


class Image:
    def __init__(self, path):
        self.path = _os.path.abspath(path)
        self.stat = _os.stat(self.path)
        with builtins.open(self.path, 'rb') as f:
            magic, off, size, count = HEAD.unpack(f.read(HEAD.size))
            if magic != MAGIC or not 0 < count <= 0xffffffff or off < HEAD.size or off > self.stat.st_size or size != self.stat.st_size - off or count > size // ENT.size:
                raise ValueError(f'invalid RomFS image header: {path}')
            f.seek(off); self.index = f.read(size)
        if len(self.index) != size: raise ValueError(f'incomplete RomFS image: {path}')
        self.count, self.pool = count, count * ENT.size
        next_child = 1
        for k in range(count):
            a, b, no, nl, t, pad = self.entry(k)
            if no + nl > size - self.pool or t > 1 or pad or (k == 0 and (t or nl)):
                raise ValueError(f'invalid RomFS image entry {k}: {path}')
            name = self.name(k)
            if k and (not nl or nl > 0x300 or b'\0' in name or b'/' in name or name in (b'.', b'..')):
                raise ValueError(f'invalid RomFS image name: {path}')
            if t:
                if a < HEAD.size or a > off or b > off - a: raise ValueError(f'invalid RomFS image file range: {path}')
            else:
                if a != next_child or a <= k or a > count or b > count - a: raise ValueError(f'invalid RomFS image tree: {path}')
                next_child += b
        if next_child != count: raise ValueError(f'unreachable RomFS image entries: {path}')
        for k in range(count):
            a, b, _, _, t, _ = self.entry(k)
            if not t and any(self.name(j - 1) >= self.name(j) for j in range(a + 1, a + b)):
                raise ValueError(f'unsorted or duplicate RomFS image names: {path}')

    def entry(self, k): return ENT.unpack_from(self.index, k * ENT.size)
    def name(self, k):
        _, _, no, nl, _, _ = self.entry(k)
        return self.index[self.pool + no:self.pool + no + nl]

    def find(self, relative):
        k = 0
        for part in relative.split('/'):
            if not part or part == '.': continue
            a, b, _, _, t, _ = self.entry(k)
            if t: raise NotADirectoryError(relative)
            lo, hi, wanted = a, a + b, _os.fsencode(part)
            while lo < hi:
                mid = (lo + hi) // 2
                if self.name(mid) < wanted: lo = mid + 1
                else: hi = mid
            if lo == a + b or self.name(lo) != wanted: raise FileNotFoundError(relative)
            k = lo
        return k

    def children(self, k):
        a, b, _, _, t, _ = self.entry(k)
        if t: raise NotADirectoryError(self.path)
        return [(_os.fsdecode(self.name(j)), j) for j in range(a, a + b)]


@functools.lru_cache(maxsize=8)
def _image(path, identity): return Image(path) if is_image(path) else None


def is_image(path):
    try:
        with builtins.open(path, 'rb') as f: return f.read(8) == MAGIC
    except (OSError, TypeError): return False


def _resolve(path):
    p = _os.path.abspath(_os.fsdecode(path))
    parts = p.split('/')
    for i, part in enumerate(parts):
        if not part.endswith('.img'): continue
        image = '/'.join(parts[:i + 1])
        try: st = _os.stat(image)
        except OSError: continue
        if not _stat.S_ISREG(st.st_mode): continue
        loaded = _image(image, (st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns))
        if loaded is not None: return loaded, '/'.join(parts[i + 1:])
    return None


def source(extraction):
    image = _Path(extraction) / 'RomFS.img'
    return Path(image if image.is_file() else _Path(extraction) / 'RomFS')


_fd_ranges = {}


class _File(io.RawIOBase):
    def __init__(self, image, k):
        self.fd = _os.open(image.path, _os.O_RDONLY)
        self.start, self.length = image.entry(k)[:2]
        self.pos = 0
        _fd_ranges[self.fd] = (self.start, self.length)
    def readable(self): return True
    def seekable(self): return True
    def fileno(self): return self.fd
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or pos < 0: raise ValueError('invalid seek')
        self.pos = pos; return pos
    def readinto(self, buffer):
        size = min(len(buffer), max(0, self.length - self.pos))
        data = _os.pread(self.fd, size, self.start + self.pos)
        buffer[:len(data)] = data; self.pos += len(data)
        return len(data)
    def close(self):
        if not self.closed:
            _fd_ranges.pop(self.fd, None); _os.close(self.fd)
        super().close()


# Type checkers see the builtin's overloads, so a reader's open(..., 'rb').read() stays bytes and 'r' stays str.
if TYPE_CHECKING:
    from builtins import open
else:
    def open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, **kwargs):
        resolved = _resolve(file) if not isinstance(file, int) else None
        if not resolved: return builtins.open(file, mode, buffering, encoding, errors, newline, **kwargs)
        if mode not in ('r', 'rb', 'rt'): raise OSError(errno.EROFS, 'RomFS images are read-only', file)
        image, relative = resolved; k = image.find(relative)
        if not image.entry(k)[4]: raise IsADirectoryError(file)
        raw = _File(image, k)
        if buffering == 0 and 'b' in mode: return raw
        reader = io.BufferedReader(raw, io.DEFAULT_BUFFER_SIZE if buffering < 0 else buffering or io.DEFAULT_BUFFER_SIZE)
        return reader if 'b' in mode else io.TextIOWrapper(reader, encoding=encoding, errors=errors, newline=newline)


def stat(path, **kwargs):
    resolved = _resolve(path)
    if not resolved: return _os.stat(path, **kwargs)
    image, relative = resolved; k = image.find(relative)
    a, b, _, _, t, _ = image.entry(k); s = image.stat
    return SimpleNamespace(st_mode=(_stat.S_IFREG | 0o444) if t else (_stat.S_IFDIR | 0o555), st_size=b if t else 0,
                           st_ino=k + 1, st_dev=s.st_dev, st_mtime=s.st_mtime, st_mtime_ns=s.st_mtime_ns,
                           st_ctime=s.st_ctime, st_ctime_ns=s.st_ctime_ns, st_atime=s.st_atime, st_atime_ns=s.st_atime_ns)


def exists(path):
    try: stat(path); return True
    except OSError: return False
def isdir(path):
    try: return _stat.S_ISDIR(stat(path).st_mode)
    except OSError: return False
def isfile(path):
    try: return _stat.S_ISREG(stat(path).st_mode)
    except OSError: return False


class _Entry:
    def __init__(self, parent, name): self.name, self.path = name, _os.path.join(parent, name)
    def stat(self, **kwargs): return stat(self.path)
    def is_dir(self, **kwargs): return isdir(self.path)
    def is_file(self, **kwargs): return isfile(self.path)
    def is_symlink(self): return False
    def inode(self): return self.stat().st_ino
    def __fspath__(self): return self.path


class _Scan:
    def __init__(self, entries): self.entries = iter(entries)
    def __iter__(self): return self
    def __next__(self): return next(self.entries)
    def __enter__(self): return self
    def __exit__(self, *args): self.close()
    def close(self): pass


def scandir(path):
    resolved = _resolve(path)
    if not resolved: return _os.scandir(path)
    image, relative = resolved
    return _Scan(_Entry(path, name) for name, _ in image.children(image.find(relative)))


def walk(top, topdown=True, onerror=None, followlinks=False):
    if not _resolve(top):
        yield from _os.walk(top, topdown=topdown, onerror=onerror, followlinks=followlinks); return
    def visit(path):
        try:
            dirs, files = [], []
            with scandir(path) as entries:
                for e in entries: (dirs if e.is_dir() else files).append(e.name)
        except OSError as error:
            if onerror: onerror(error)
            return
        if topdown: yield str(path), dirs, files
        for name in dirs: yield from visit(_os.path.join(path, name))
        if not topdown: yield str(path), dirs, files
    yield from visit(top)


class Path(type(_Path())):
    def stat(self, **kwargs): return stat(self, **kwargs)
    def exists(self, **kwargs): return exists(self)
    def is_dir(self, **kwargs): return isdir(self)
    def is_file(self, **kwargs): return isfile(self)
    def read_bytes(self):
        with self.open('rb') as f: return f.read()
    def read_text(self, encoding=None, errors=None, newline=None):
        with self.open('r', encoding=encoding, errors=errors, newline=newline) as f: return f.read()
    def open(self, mode='r', buffering=-1, encoding=None, errors=None, newline=None):
        return open(self, mode, buffering, encoding, errors, newline)
    def iterdir(self):
        with scandir(self) as entries:
            for e in entries: yield self / e.name
    def glob(self, pattern, **kwargs):
        if not _resolve(self): yield from super().glob(pattern, **kwargs); return
        if '/' not in pattern:
            yield from (p for p in self.iterdir() if fnmatch.fnmatchcase(p.name, pattern)); return
        for directory, dirs, files in walk(self):
            for name in dirs + files:
                p = Path(directory) / name; rel = p.relative_to(self).as_posix()
                if fnmatch.fnmatchcase(rel, pattern) or (pattern.startswith('**/') and fnmatch.fnmatchcase(rel, pattern[3:])): yield p
    def rglob(self, pattern, **kwargs):
        if not _resolve(self): yield from super().rglob(pattern, **kwargs); return
        yield from self.glob('**/' + str(pattern))


class _PathOps:
    exists = staticmethod(exists); isdir = staticmethod(isdir); isfile = staticmethod(isfile)
    getsize = staticmethod(lambda path: stat(path).st_size)
    def __getattr__(self, name): return getattr(_os.path, name)
class _OS:
    path = _PathOps()
    stat = staticmethod(stat); scandir = staticmethod(scandir); walk = staticmethod(walk)
    @staticmethod
    def listdir(path):
        with scandir(path) as entries: return [e.name for e in entries]
    def __getattr__(self, name): return getattr(_os, name)
os = _OS()


class _Map:
    """mmap-compatible bounded buffer. Python 3.12's buffer protocol keeps archive parsing zero-copy."""
    def __init__(self, fd, start, length):
        self.length, self.pos = length, 0
        self.delta = start % _mmap.ALLOCATIONGRANULARITY
        self.mapping = _mmap.mmap(fd, length + self.delta, access=_mmap.ACCESS_READ, offset=start - self.delta)
        self.view = memoryview(self.mapping)[self.delta:self.delta + length]
    def __buffer__(self, flags): return self.view
    def __len__(self): return self.length
    def __getitem__(self, key):
        value = self.view[key]
        return value.tobytes() if isinstance(key, slice) else value
    def __contains__(self, value): return self.find(value) >= 0
    def find(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        found = self.mapping.find(value, self.delta + a, self.delta + b)
        return found - self.delta if found >= 0 else -1
    def rfind(self, value, start=0, end=None):
        a, b, _ = slice(start, end).indices(self.length)
        found = self.mapping.rfind(value, self.delta + a, self.delta + b)
        return found - self.delta if found >= 0 else -1
    def index(self, value, start=0, end=None):
        found = self.find(value, start, end)
        if found < 0: raise ValueError('subsection not found')
        return found
    def size(self): return self.length
    def tell(self): return self.pos
    def seek(self, offset, whence=0):
        pos = offset + (self.pos if whence == 1 else self.length if whence == 2 else 0)
        if whence not in (0, 1, 2) or not 0 <= pos <= self.length: raise ValueError('invalid mmap seek')
        self.pos = pos
    def read(self, size=-1):
        end = self.length if size < 0 else min(self.length, self.pos + size)
        data = self[self.pos:end]; self.pos = end; return data
    def close(self): self.view.release(); self.mapping.close()
    def __enter__(self): return self
    def __exit__(self, *args): self.close()


class _Mmap:
    @staticmethod
    def mmap(fd, length, *args, **kwargs):
        if fd not in _fd_ranges: return _mmap.mmap(fd, length, *args, **kwargs)
        if sys.version_info < (3, 12): raise RuntimeError('RomFS image archive scans require Python 3.12 or newer')
        if args or kwargs.get('access', _mmap.ACCESS_READ) != _mmap.ACCESS_READ: raise ValueError('RomFS mappings are read-only')
        start, size = _fd_ranges[fd]; offset = kwargs.get('offset', 0)
        if offset < 0 or offset > size or (length and length > size - offset): raise ValueError('mapping exceeds RomFS file')
        return _Map(fd, start + offset, length or size - offset)
    def __getattr__(self, name): return getattr(_mmap, name)
mmap = _Mmap()


if __name__ == '__main__':
    try:
        root = Path(sys.argv[1]); marker = sys.argv[2] if len(sys.argv) > 2 else ''
        if not root.is_dir() or not (root / marker).exists(): raise FileNotFoundError(f'missing RomFS marker {marker!r} in {root}')
    except (OSError, ValueError, IndexError, struct.error) as error: sys.exit(f'[RED] {error}')
