#!/usr/bin/env python3
# Pack the IPA (a zip archive) straight from its source folders: the app Xcode built, the iOS shader libraries, the add-on content and the RomFS, each under its own path in the archive,
# with no Payload/ copy first. Replaces zip -qry -n with the same entries: folders, Unix modes (the executable's x bit), symlinks as links, DOS times, the UTF-8 flag for non-ASCII names;
# deflate level 6, a file whose deflate is not smaller is stored (zip's rule), and names ending in a --store suffix are always stored (the profile's zip_store, compared without regard to case).
# Speed (Info-ZIP managed about 100 MB/s on the T7): files up to BIG are read, and deflated, by a thread pool (zlib releases the GIL) while this thread writes them in order;
# bigger files are streamed here, their CRC patched into the local header afterwards (no data descriptors). Zip64 fields wherever a size, an offset or the entry count passes its 32 / 16-bit field.
# Usage: pack_ipa.py <out.ipa> [--store .a:.b] [--skip <archive path>]... <archive path>=<source file or folder>...
#   Mappings are packed in command-line order; put the files that change least first, and the app last
#   --skip: an archive path left out of the walk of the folder that holds it (folders a stale app build still has, the signature); a mapping of its own still adds it
#   --skip-suffix: files whose names end so are left out of every walk (build_ipa.sh: the pulling vertex libraries _bp.metallib when the profile's pso_mtl4 is off, unless IOS_PULL overrides it)
#   Writes <out.ipa>.partial and renames it at the end; prints "pack: N entries, X bytes" first (the NSP script's progress total, X = the files' bytes) and a summary line last
#   Repeat packs reuse a file when its size, mtime_ns, ctime_ns and inode are unchanged (true on APFS). A tool that rewrites in place and restores mtime defeats this if ctime also stays
#   unchanged; --no-reuse forces a full pack. The index holds only source metadata and compression settings; ZIP metadata comes from the old IPA.
#   Only mappings whose source folder is on APFS reuse files; other mappings pack their files again. The kept run needs an APFS clone of the old IPA; if cloning fails, eligible files are copied compressed.
#   Reused bytes are not checked against their CRC: damage inside the old IPA carries over. Use --no-reuse or delete <out.ipa>.idx to rebuild
import argparse, collections, concurrent.futures as cf, ctypes, json, os, stat, struct, sys, time, zipfile, zlib

LEVEL = 6
IDX_VERSION = 3
BIG = 256 << 20      # files above this are streamed by the writer; smaller ones go through the pool
WINDOW = 2 << 30     # input bytes in the pool at once
QUEUE = 4096         # entries in the pool at once (folders of tiny files)
CHUNK = 16 << 20
L32, L16 = 0xFFFFFFFF, 0xFFFF
DIR_MODE = stat.S_IFDIR | 0o755   # a parent folder no source folder stands for (Payload/, msl/); its time is 1980-01-01, so it is the same in every pack
Entry = collections.namedtuple('Entry', 'name path st root')   # st None: a synthetic parent; root: the mapping's source folder


def entries(maps, skip, skip_suffix=()):
    """The archive's entries in order: for each mapping its parent folders (unless an earlier entry gave them), its root, then its tree depth first, sorted by name,
    without the skipped paths and without other mappings' roots (each adds its own). Parents needed before their mapping are synthetic folders with fixed metadata."""
    roots, out, seen, parents = {a for a, _ in maps}, [], set(), set()
    def add(name, path, st):
        if name in parents and st is not None and stat.S_ISDIR(st.st_mode): return   # written already, as a parent folder
        if name in seen: sys.exit(f'[RED] pack_ipa: {name} twice in the archive')
        seen.add(name); out.append(Entry(name, path, st, src))
    def walk(d, arc):
        with os.scandir(d) as it: es = sorted(it, key=lambda e: e.name)
        for e in es:
            name = f'{arc}/{e.name}'
            if name in skip or name in roots or (skip_suffix and e.name.endswith(skip_suffix) and not e.is_dir(follow_symlinks=False)): continue
            st = e.stat(follow_symlinks=False)
            if stat.S_ISDIR(st.st_mode): add(name + '/', e.path, st); walk(e.path, name)
            elif stat.S_ISREG(st.st_mode) or stat.S_ISLNK(st.st_mode): add(name, e.path, st)
            else: sys.exit(f'[RED] pack_ipa: neither a file, a folder nor a symlink: {e.path}')
    for arc, src in maps:
        parts = arc.split('/')
        for i in range(1, len(parts)):
            p = '/'.join(parts[:i])
            if p + '/' not in seen:
                add(p + '/', None, None); parents.add(p + '/')
        source_stat = os.stat(src)
        if stat.S_ISREG(source_stat.st_mode): add(arc, src, source_stat)
        else: add(arc + '/', src, source_stat); walk(src, arc)
    return out


def load(path, deflate):   # in the pool: (method, CRC, the bytes to write, uncompressed length)
    with open(path, 'rb') as f: data = f.read()
    crc, usize = zlib.crc32(data), len(data)
    if deflate and data:
        c = zlib.compressobj(LEVEL, zlib.DEFLATED, -15); z = c.compress(data) + c.flush()
        if len(z) < len(data): return 8, crc, z, usize
    return 0, crc, data, usize


def dos_time(t):
    y, mo, d, h, mi, s = time.localtime(t)[:6]
    if y < 1980: y, mo, d, h, mi, s = 1980, 1, 1, 0, 0, 0
    return (h << 11) | (mi << 5) | (s // 2), ((y - 1980) << 9) | (mo << 5) | d


def header(e, method, crc, csize, usize, z64=False):   # a local header: (its bytes, name bytes, flags, time, date)
    nb = e.name.encode('utf-8', 'surrogateescape'); fl = 0 if e.name.isascii() else 0x800
    t, d = dos_time(e.st.st_mtime if e.st else 0)
    ext = struct.pack('<HHQQ', 1, 16, usize, csize) if z64 else b''
    return (struct.pack('<IHHHHHIIIHH', 0x04034B50, 45 if z64 else 20 if method == 8 else 10, fl, method, t, d, crc, L32 if z64 else csize, L32 if z64 else usize, len(nb), len(ext))
            + nb + ext, nb, fl, t, d)


def clone(old, dst):   # clone the checked inode, even if its path was replaced; AT_FDCWD is -2 on Darwin
    try: return ctypes.CDLL(None, use_errno=True).fclonefileat(old.fileno(), -2, os.fsencode(dst), 0) == 0
    except (AttributeError, OSError): return False


class Statfs64(ctypes.Structure):   # Darwin's statfs64 layout from sys/mount.h, including the fields after f_fstypename
    _fields_ = [('f_bsize', ctypes.c_uint32), ('f_iosize', ctypes.c_int32), ('counts', ctypes.c_uint64 * 5), ('f_fsid', ctypes.c_int32 * 2),
                ('f_owner', ctypes.c_uint32), ('f_type', ctypes.c_uint32), ('f_flags', ctypes.c_uint32), ('f_fssubtype', ctypes.c_uint32),
                ('f_fstypename', ctypes.c_char * 16), ('f_mntonname', ctypes.c_char * 1024), ('f_mntfromname', ctypes.c_char * 1024), ('reserved', ctypes.c_uint32 * 8)]


def fs_type(root):
    if sys.platform != 'darwin': return b''
    fs = Statfs64()
    try:
        if ctypes.CDLL(None).statfs64(os.fsencode(root), ctypes.byref(fs)) == 0: return fs.f_fstypename
    except (AttributeError, OSError): pass
    return b''   # unknown identity: do not reuse


class Zip:
    def __init__(self, path, keep=0):   # keep: the bytes of a clone at path that stay (the leading entries kept)
        self.f = open(path, 'r+b' if keep else 'wb', buffering=1 << 20)
        if keep: self.f.seek(keep)
        self.pos, self.cd, self.written = keep, [], 0   # written: the bytes this pack writes (entries, header patches, the central directory; main adds the index)
        self.stats = collections.Counter()

    def w(self, b):
        self.f.write(b); self.pos += len(b); self.written += len(b)

    def local(self, e, method, crc, csize, usize, z64=False):   # writes the local header; returns (its offset, name bytes, flags, time, date)
        b, nb, fl, t, d = header(e, method, crc, csize, usize, z64); off = self.pos; self.w(b)
        return off, nb, fl, t, d

    def central(self, e, h, method, crc, csize, usize):
        off, nb, fl, t, d = h
        z = [v for v in (usize, csize, off) if v >= L32]   # zip64 extra: the fields that do not fit, in this order
        ext = struct.pack(f'<HH{len(z)}Q', 1, 8 * len(z), *z) if z else b''
        mode = e.st.st_mode if e.st else DIR_MODE
        self.cd.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 0x031E, 45 if z else 20 if method == 8 else 10, fl, method, t, d, crc, min(csize, L32), min(usize, L32),
                                   len(nb), len(ext), 0, 0, 0, (mode << 16) | (0x10 if stat.S_ISDIR(mode) else 0), min(off, L32)) + nb + ext)
        self.stats['deflated' if method == 8 else 'stored'] += usize

    def kept(self, e, info, h):
        self.central(e, (info.header_offset,) + h[1:], info.compress_type, info.CRC, info.compress_size, info.file_size)
        if e.st and stat.S_ISREG(e.st.st_mode): self.stats['reused'] += 1

    def put(self, e, method, crc, data, usize):   # an entry whose bytes are at hand
        h = self.local(e, method, crc, len(data), usize)
        if data: self.w(data)
        self.central(e, h, method, crc, len(data), usize)

    def other(self, e):   # a folder or a symlink
        if e.st and stat.S_ISLNK(e.st.st_mode):
            t = os.readlink(e.path).encode('utf-8', 'surrogateescape'); self.put(e, 0, zlib.crc32(t), t, len(t))
        else: self.put(e, 0, 0, b'', 0)

    def patch(self, at, b):
        self.f.seek(at); self.f.write(b); self.written += len(b)

    def stream(self, e, deflate):   # a big file: the local header first, its CRC (and a deflated size) patched in afterwards
        usize = e.st.st_size; z64 = usize >= L32   # a deflate that is not smaller is redone stored, so the deflated size stays below usize
        h = self.local(e, 8 if deflate else 0, 0, 0 if deflate else usize, usize, z64)
        crc, csize, read, c = 0, 0, 0, zlib.compressobj(LEVEL, zlib.DEFLATED, -15) if deflate else None
        with open(e.path, 'rb') as f:
            while b := f.read(CHUNK):
                read += len(b); crc = zlib.crc32(b, crc)
                if c: b = c.compress(b)
                self.w(b); csize += len(b)
            if c: b = c.flush(); self.w(b); csize += len(b)
        if read != usize: sys.exit(f'[RED] pack_ipa: {e.path} changed size while it was packed')
        if c and csize >= usize:   # not smaller: stored instead
            self.f.seek(h[0]); self.f.truncate(); self.pos = h[0]; return self.stream(e, False)
        self.patch(h[0] + 14, struct.pack('<I', crc) + (b'' if z64 else struct.pack('<II', csize, usize)))
        if z64: self.patch(h[0] + 30 + len(h[1]) + 4, struct.pack('<QQ', usize, csize))
        self.f.seek(self.pos)
        self.central(e, h, 8 if c else 0, crc, csize, usize)

    def reuse(self, e, info, h, old):   # copy already compressed bytes, after the checked local header
        method, crc, csize, usize = info.compress_type, info.CRC, info.compress_size, info.file_size
        nl, el = struct.unpack_from('<HH', h[0], 26)
        left, at = csize, info.header_offset + 30 + nl + el
        h = self.local(e, method, crc, csize, usize, usize >= L32 or csize >= L32)
        while left:
            b = os.pread(old.fileno(), min(CHUNK, left), at)
            if not b: sys.exit(f'[RED] pack_ipa: the old IPA ended while {e.name} was copied from it')
            self.w(b); left -= len(b); at += len(b)
        self.central(e, h, method, crc, csize, usize)
        self.stats['reused'] += 1

    def close(self):   # the central directory and the end records (zip64 ones when the count, the size or the offset does not fit)
        self.cd_off, n = self.pos, len(self.cd); blob = b''.join(self.cd); self.w(blob)
        if n >= L16 or self.cd_off >= L32 or len(blob) >= L32:
            z = self.pos
            self.w(struct.pack('<IQHHIIQQQQ', 0x06064B50, 44, 0x031E, 45, 0, 0, n, n, len(blob), self.cd_off))
            self.w(struct.pack('<IIQI', 0x07064B50, 0, z, 1))
        self.w(struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, min(n, L16), min(n, L16), min(len(blob), L32), min(self.cd_off, L32), 0))
        self.f.close()


def load_index(out):   # (the old IPA opened, its index) when <out>.idx describes the IPA there now and this deflate level, else None
    old = None
    try:
        with open(out + '.idx') as f: idx = json.load(f)
        old = open(out, 'rb'); st = os.fstat(old.fileno())
        if (isinstance(idx, dict) and idx.get('version') == IDX_VERSION and idx.get('level') == LEVEL and idx.get('zlib') == zlib.ZLIB_RUNTIME_VERSION
                and idx.get('ipa') == [st.st_size, st.st_mtime_ns] and isinstance(idx.get('files'), dict)):
            with zipfile.ZipFile(old) as archive: infos = {i.filename: i for i in archive.infolist()}
            return old, idx, infos
    except (OSError, ValueError, zipfile.BadZipFile): pass
    if old: old.close()
    return None


def fingerprint(e, deflate):
    st = e.st
    return [st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_ino, deflate]


def match(e, row, info, old, idx, apfs):   # one header comparison for both the kept run and later compressed copies
    if info is None: return None
    mode, data = e.st.st_mode if e.st else DIR_MODE, b''
    if not stat.S_ISDIR(mode) and not apfs[e.root]: return None
    if stat.S_ISREG(mode):
        if idx['files'].get(e.name) != row or info.file_size != e.st.st_size or info.compress_type not in (0, 8): return None
        method, crc, csize, usize = info.compress_type, info.CRC, info.compress_size, info.file_size
    else:
        if stat.S_ISLNK(mode): data = os.readlink(e.path).encode('utf-8', 'surrogateescape')
        method, crc, csize, usize = 0, zlib.crc32(data), len(data), len(data)
        if (info.compress_type, info.CRC, info.compress_size, info.file_size) != (method, crc, csize, usize): return None
    h = header(e, method, crc, csize, usize, usize >= L32 or csize >= L32)
    if info.header_offset < 0 or info.header_offset + len(h[0]) + csize > idx['ipa'][0]: return None
    if os.pread(old.fileno(), len(h[0]) + len(data), info.header_offset) != h[0] + data: return None
    return h


def prefix(es, files, infos, old, idx, apfs):
    keep, pos = [], 0
    for e in es:
        info = infos.get(e.name)
        if info is None or info.header_offset != pos: break
        h = match(e, files.get(e.name), info, old, idx, apfs)
        if h is None: break
        keep.append((e, info, h)); pos += len(h[0]) + info.compress_size
    return keep, pos


def main():
    ap = argparse.ArgumentParser(description='Pack an IPA (zip) from files and folders mapped to archive paths, in parallel')
    ap.add_argument('out'); ap.add_argument('maps', nargs='+', metavar='archive_path=source')
    ap.add_argument('--store', default='', help='suffixes stored without compressing, separated by ":"')
    ap.add_argument('--skip', action='append', default=[], help='an archive path left out of the folder walk that would add it')
    ap.add_argument('--skip-suffix', action='append', default=[], help='files whose names end so are left out of every folder walk')
    ap.add_argument('--no-reuse', action='store_true', help='pack every file again, without copying from the old IPA')
    a = ap.parse_args()
    maps = []
    for m in a.maps:
        arc, sep, src = m.partition('=')
        if not sep or not arc.strip('/') or not (os.path.isdir(src) or os.path.isfile(src)): sys.exit(f'[RED] pack_ipa: not <archive path>=<file or folder>: {m}')
        maps.append((arc.strip('/'), src))
    store = tuple(s.lower() for s in a.store.split(':') if s)
    t0 = time.time()
    if len({arc for arc, _ in maps}) != len(maps): sys.exit('[RED] pack_ipa: mapping twice in the archive')
    es = entries(maps, {s.strip('/') for s in a.skip}, tuple(a.skip_suffix))
    total = sum(e.st.st_size for e in es if e.st and stat.S_ISREG(e.st.st_mode))
    print(f'pack: {len(es)} entries, {total} bytes', flush=True)
    apfs = {root: fs_type(root) == b'apfs' for root in {src for _, src in maps}}
    disabled = '; '.join(f'reuse off for {arc}: not APFS' for arc, src in maps if not apfs[src])
    flags = [not e.name.lower().endswith(store) if store else True for e in es]   # each file's deflate flag
    files = {e.name: fingerprint(e, f) for e, f in zip(es, flags) if e.st and stat.S_ISREG(e.st.st_mode)}
    part, tmp, ipart = a.out + '.partial', a.out + '.clone', a.out + '.idx.partial'
    for path in (part, tmp, ipart):
        if os.path.lexists(path): os.unlink(path)
    old, idx, infos = (None, None, {}) if a.no_reuse else load_index(a.out) or (None, None, {})
    keep, kept = prefix(es, files, infos, old, idx, apfs) if old else ([], 0)
    z = None
    try:
        if kept:
            if clone(old, tmp):
                with open(tmp, 'r+b') as f: f.truncate(kept)
                os.replace(tmp, part)
            else: keep, kept = [], 0   # no clone: everything is written, files still copied compressed
        if os.path.lexists(tmp): os.unlink(tmp)
        print(f'pack: keeping {kept} bytes', flush=True)
        z = Zip(part, kept)
        for k in keep: z.kept(*k)
        pend, inflight = collections.deque(), 0
        def one():   # write the oldest pending entry
            kind, e, value = pend.popleft()
            if kind == 'other': z.other(e); return 0
            if kind == 'reuse': z.reuse(e, *value, old); return 0
            method, crc, data, usize = value.result()
            if usize != e.st.st_size: sys.exit(f'[RED] pack_ipa: {e.path} changed size while it was packed')
            z.put(e, method, crc, data, usize); return usize
        with cf.ThreadPoolExecutor(os.cpu_count()) as pool:
            for e, deflate in zip(es[len(keep):], flags[len(keep):]):
                if not (e.st and stat.S_ISREG(e.st.st_mode)): pend.append(('other', e, None)); continue
                info = infos.get(e.name)
                h = match(e, files[e.name], info, old, idx, apfs) if old else None
                if h is not None: pend.append(('reuse', e, (info, h))); continue
                if e.st.st_size > BIG:
                    while pend: inflight -= one()
                    z.stream(e, deflate); continue
                while pend and (len(pend) >= QUEUE or inflight + e.st.st_size > WINDOW): inflight -= one()
                pend.append(('load', e, pool.submit(load, e.path, deflate))); inflight += e.st.st_size
            while pend: inflight -= one()
        z.close()
        st = os.stat(part)   # bind the index to this file before publishing; rename preserves its mtime
        os.replace(part, a.out)
    except BaseException:
        if z: z.f.close()
        for path in (part, tmp):
            if os.path.lexists(path): os.unlink(path)
        if old: old.close()
        raise
    if old: old.close()
    try:   # the index only after the rename: an IPA it does not describe (size, time) makes it ignored
        blob = json.dumps({'version': IDX_VERSION, 'level': LEVEL, 'zlib': zlib.ZLIB_RUNTIME_VERSION, 'ipa': [st.st_size, st.st_mtime_ns], 'files': files})
        with open(ipart, 'w') as f: f.write(blob)
        os.replace(ipart, a.out + '.idx'); z.written += len(blob)
    except OSError as x: print(f'[RED] pack_ipa: index not written, the next pack packs every file again: {x}', flush=True)
    s, dt = z.stats, time.time() - t0
    print(f'packed: {len(es)} entries, stored {s["stored"] / 2**30:.2f} GB, deflated {s["deflated"] / 2**30:.2f} GB, archive {st.st_size / 2**30:.2f} GB, '
          f'kept {kept / 2**30:.2f} GB, reused {s["reused"]} files in {dt:.0f} s, wrote {z.written / 2**30:.2f} GB' + (f'; {disabled}' if disabled else ''), flush=True)


if __name__ == '__main__': main()
