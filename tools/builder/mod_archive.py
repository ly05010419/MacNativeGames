"""Archives a mod comes in (S5.4 C2; S5.5 C2, C4). The model: no Tk.

extract(archive, into, cancel) fills into (which the caller removes in every case) or raises Refused:
- .zip with the standard zipfile, counting the entries and the bytes actually written.
- .7z and .rar with the system's bsdtar (/usr/bin/tar), never through a shell and never with -P (without it bsdtar
  refuses '..' and extracting through a link). First the listing: `tar -tf` for the exact names and `tar -tvf` for each
  entry's type and size, matched line by line; only regular files and folders; the declared sizes summed against
  MAX_BYTES and the free space. Then the extraction, through a wrapper that sets RLIMIT_FSIZE (FILE_BYTES) and puts
  SIGXFSZ back to its default (CPython ignores it, and execv would keep it ignored), so a file reaching the cap stops
  tar with -SIGXFSZ (zip_big). Then a walk of into: a link, a name the listing did not have, or a total over MAX_BYTES
  is refused. Each tar runs in its own session, polled; a cancel or a timeout kills its group (Refused 'cancelled' or
  'zip_bad').
- .tkcl, a TKMM package, is refused with tkmm_package; .bnp, a BCML package, with bcml_package (S9.2); any other
  suffix with archive_kind.
Every archive: absolute names, '..', a backslash start, a newline, names APFS would merge (letter case or Unicode form)
and a file named like a folder another entry needs are refused; __MACOSX/ (Finder's Compress) is skipped."""
import os
from pathlib import Path, PurePosixPath
import shutil
import signal
import stat
import sys
import unicodedata
import zipfile

from tools.builder import cancellable
from tools.builder.mod_find import SKIPPED, Refused

MAX_ENTRIES, MAX_BYTES = 100000, 64 << 30
FILE_BYTES = MAX_BYTES                  # RLIMIT_FSIZE for tar: one file never passes the total cap
MARGIN = 1 << 30
TAR = '/usr/bin/tar'
LIST_S, EXTRACT_S = 60.0, 600.0
TAR_SUFFIXES = ('.7z', '.rar')
LIMIT = ('import os, resource, signal, sys; n = int(sys.argv[1]); resource.setrlimit(resource.RLIMIT_FSIZE, (n, n)); '
         'signal.signal(signal.SIGXFSZ, signal.SIG_DFL); os.execv(sys.argv[2], sys.argv[2:])')


def _key(name):
    """A name as APFS compares it: letter case and Unicode form (NFC or NFD) make no difference."""
    return unicodedata.normalize('NFD', name.rstrip('/')).casefold()


def _checked(entries):
    """entries: (name, is_dir) without __MACOSX/; the APFS keys of the names, or Refused."""
    if len(entries) > MAX_ENTRIES:
        raise Refused('zip_entries', str(len(entries)))
    seen, parents, files = set(), set(), {}
    for name, is_dir in entries:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or name.startswith('\\') or '\n' in name or '�' in name:
            raise Refused('zip_path', name)
        key = _key(name)
        if key in seen:
            raise Refused('zip_case', name)
        seen.add(key)
        parents.update(str(p) for p in PurePosixPath(key).parents if str(p) != '.')
        if not is_dir:
            files[key] = name
    clash = sorted(set(files) & parents)
    if clash:                                   # a file and a folder of one name: one cannot be written
        raise Refused('zip_path', files[clash[0]])
    return seen | parents                       # what may exist on disk after extracting: entries and their folders


def _skipped(name):
    return PurePosixPath(name).parts[:1] == (SKIPPED,)


def extract(archive, into, cancel=None):
    archive, into = Path(archive), Path(into)
    suffix = archive.suffix.lower()
    if suffix == '.tkcl':
        raise Refused('tkmm_package', archive.name)
    if suffix == '.bnp':
        raise Refused('bcml_package', archive.name)
    if suffix == '.zip':
        return _zip(archive, into)
    if suffix in TAR_SUFFIXES:
        return _tar(archive, into, cancel)
    raise Refused('archive_kind', archive.suffix)


def _space(into, declared, archive):
    into.mkdir(parents=True, exist_ok=True)     # the library's folder may not exist before its first mod
    if shutil.disk_usage(into).free < declared + MARGIN:
        raise Refused('zip_space', archive.name)


def _zip(archive, into):
    try:
        with zipfile.ZipFile(archive) as z:
            entries = z.infolist()
            if len(entries) > MAX_ENTRIES:
                raise Refused('zip_entries', str(len(entries)))
            entries = [e for e in entries if not _skipped(e.filename)]
            for e in entries:
                if stat.S_ISLNK(e.external_attr >> 16):
                    raise Refused('zip_link', e.filename)
            _checked([(e.filename, e.is_dir()) for e in entries])
            _space(into, sum(e.file_size for e in entries), archive)
            written = 0
            for e in entries:
                target = into / e.filename
                if e.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(e) as src, open(target, 'wb') as dst:
                    while chunk := src.read(1 << 20):
                        written += len(chunk)
                        if written > MAX_BYTES:
                            raise Refused('zip_big', archive.name)
                        dst.write(chunk)
    except zipfile.BadZipFile as e:
        raise Refused('zip_bad', str(e)) from None
    return into


def _run(argv, cancel, timeout_s):
    """cancellable.run, its stops as this module's refusals."""
    try:
        return cancellable.run(argv, cancel, timeout_s)
    except cancellable.Stopped as stop:
        raise Refused('cancelled', '') if stop.reason == 'cancelled' else \
            Refused('zip_bad', 'timeout' if stop.reason == 'timeout' else stop.detail) from None


def _last(stderr):
    return cancellable.last_line(stderr)


def _listing(archive, cancel):
    """[(name, is_dir, size)] from tar -tf and tar -tvf, or Refused."""
    lists = []
    for flags in ('-tf', '-tvf'):
        code, out, err = _run([TAR, flags, archive], cancel, LIST_S)
        if code != 0:
            raise Refused('zip_bad', _last(err))
        try:
            lists.append(out.decode().splitlines())
        except UnicodeDecodeError:
            raise Refused('zip_path', 'a name that is not UTF-8') from None
    names, verbose = lists
    if len(names) != len(verbose):
        raise Refused('zip_bad', 'the listings differ')
    entries = []
    for name, line in zip(names, verbose):
        fields = line.split(None, 8)
        if len(fields) < 9 or not fields[4].isdigit():
            raise Refused('zip_bad', line[:120])
        if fields[0][:1] not in '-d':           # a link, hard link, device, fifo or socket
            raise Refused('zip_link', name)
        if fields[8] != name:
            raise Refused('zip_bad', 'the listings differ')
        entries.append((name, fields[0][0] == 'd', int(fields[4])))
    return entries


def _tar(archive, into, cancel):
    entries = [e for e in _listing(archive, cancel) if not _skipped(e[0])]
    keys = _checked([(name, is_dir) for name, is_dir, _ in entries])
    declared = sum(size for _, _, size in entries)
    if declared > MAX_BYTES:
        raise Refused('zip_big', archive.name)
    _space(into, declared, archive)
    code, _, err = _run([sys.executable, '-I', '-c', LIMIT, str(FILE_BYTES), TAR, '-xf', archive, '-C', into,
                         '--no-same-owner', '--no-same-permissions', '--exclude', SKIPPED], cancel, EXTRACT_S)
    if code == -signal.SIGXFSZ:
        raise Refused('zip_big', archive.name)
    if code != 0:
        raise Refused('zip_bad', _last(err))
    total = 0
    for folder, dirs, files in os.walk(into):
        for name in dirs + files:
            path = Path(folder) / name
            relative = path.relative_to(into).as_posix()
            if path.is_symlink():
                raise Refused('zip_link', relative)
            if _key(relative) not in keys:
                raise Refused('zip_path', relative)
            if name in files:
                total += path.lstat().st_size
    if total > MAX_BYTES:
        raise Refused('zip_big', archive.name)
    return into
