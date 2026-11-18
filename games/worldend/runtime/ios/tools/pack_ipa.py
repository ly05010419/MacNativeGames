#!/usr/bin/env python3
# Pack the IPA (a zip archive) straight from its source folders: the app Xcode built, the iOS shader libraries, the add-on content and the RomFS, each under its own path in the archive,
# with no Payload/ copy first. Replaces zip -qry -n with the same entries: folders, Unix modes (the executable's x bit), symlinks as links, DOS times, the UTF-8 flag for non-ASCII names;
# deflate level 6, a file whose deflate is not smaller is stored (zip's rule), and names ending in a --store suffix are always stored (the profile's zip_store, compared without regard to case).
# Speed (Info-ZIP managed about 100 MB/s on the T7): files up to BIG are read, and deflated, by a thread pool (zlib releases the GIL) while this thread writes them in order;
# bigger files are streamed here, their CRC patched into the local header afterwards (no data descriptors). Zip64 fields wherever a size, an offset or the entry count passes its 32 / 16-bit field.
# Usage: pack_ipa.py <out.ipa> [--store .a:.b] [--skip <archive path>]... <archive path>=<source folder>...
#   --skip: an archive path left out of the walk of the folder that holds it (folders a stale app build still has, the signature); a mapping of its own still adds it
#   Writes <out.ipa>.partial and renames it at the end; prints "pack: N entries, X bytes" first (the NSP script's progress total, X = the files' bytes) and a summary line last
import argparse, collections, concurrent.futures as cf, os, stat, struct, sys, time, zlib

BIG = 256 << 20      # files above this are streamed by the writer; smaller ones go through the pool
WINDOW = 2 << 30     # input bytes in the pool at once
QUEUE = 4096         # entries in the pool at once (folders of tiny files)
CHUNK = 16 << 20
L32, L16 = 0xFFFFFFFF, 0xFFFF
DIR_MODE = stat.S_IFDIR | 0o755   # a parent folder no source folder stands for (Payload/, msl/)
Entry = collections.namedtuple('Entry', 'name path st')   # st None: such a parent folder


def entries(maps, skip):
    """The archive's entries in order: for each mapping its parent folders (unless an earlier entry gave them), its root, then its tree depth first, sorted by name,
    without the skipped paths and without other mappings' roots (each adds its own)"""
    roots, out, seen = {a for a, _ in maps}, [], set()
    def add(name, path, st):
        if name in seen: sys.exit(f'[RED] pack_ipa: {name} twice in the archive')
        seen.add(name); out.append(Entry(name, path, st))
    def walk(d, arc):
        with os.scandir(d) as it: es = sorted(it, key=lambda e: e.name)
        for e in es:
            name = f'{arc}/{e.name}'
            if name in skip or name in roots: continue
            st = e.stat(follow_symlinks=False)
            if stat.S_ISDIR(st.st_mode): add(name + '/', e.path, st); walk(e.path, name)
            elif stat.S_ISREG(st.st_mode) or stat.S_ISLNK(st.st_mode): add(name, e.path, st)
            else: sys.exit(f'[RED] pack_ipa: neither a file, a folder nor a symlink: {e.path}')
    for arc, src in maps:
        parts = arc.split('/')
        for i in range(1, len(parts)):
            p = '/'.join(parts[:i]) + '/'
            if p not in seen: add(p, None, None)
        add(arc + '/', src, os.stat(src)); walk(src, arc)
    return out


def load(path, deflate):   # in the pool: (method, CRC, the bytes to write)
    with open(path, 'rb') as f: data = f.read()
    crc = zlib.crc32(data)
    if deflate and data:
        c = zlib.compressobj(6, zlib.DEFLATED, -15); z = c.compress(data) + c.flush()
        if len(z) < len(data): return 8, crc, z
    return 0, crc, data


def dos_time(t):
    y, mo, d, h, mi, s = time.localtime(t)[:6]
    if y < 1980: y, mo, d, h, mi, s = 1980, 1, 1, 0, 0, 0
    return (h << 11) | (mi << 5) | (s // 2), ((y - 1980) << 9) | (mo << 5) | d


class Zip:
    def __init__(self, path):
        self.f, self.pos, self.cd, self.now = open(path, 'wb', buffering=1 << 20), 0, [], time.time()
        self.stats = collections.Counter()

    def w(self, b):
        self.f.write(b); self.pos += len(b)

    def local(self, e, method, crc, csize, usize, z64=False):   # the local header; returns (its offset, name bytes, flags, time, date)
        nb = e.name.encode('utf-8', 'surrogateescape'); fl = 0 if e.name.isascii() else 0x800
        t, d = dos_time(e.st.st_mtime if e.st else self.now)
        ext = struct.pack('<HHQQ', 1, 16, usize, csize) if z64 else b''
        off = self.pos
        self.w(struct.pack('<IHHHHHIIIHH', 0x04034B50, 45 if z64 else 20 if method == 8 else 10, fl, method, t, d, crc, L32 if z64 else csize, L32 if z64 else usize, len(nb), len(ext)) + nb + ext)
        return off, nb, fl, t, d

    def central(self, e, h, method, crc, csize, usize):
        off, nb, fl, t, d = h
        z = [v for v in (usize, csize, off) if v >= L32]   # zip64 extra: the fields that do not fit, in this order
        ext = struct.pack(f'<HH{len(z)}Q', 1, 8 * len(z), *z) if z else b''
        mode = e.st.st_mode if e.st else DIR_MODE
        self.cd.append(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 0x031E, 45 if z else 20 if method == 8 else 10, fl, method, t, d, crc, min(csize, L32), min(usize, L32),
                                   len(nb), len(ext), 0, 0, 0, (mode << 16) | (0x10 if stat.S_ISDIR(mode) else 0), min(off, L32)) + nb + ext)

    def put(self, e, method, crc, data, usize):   # an entry whose bytes are at hand
        h = self.local(e, method, crc, len(data), usize)
        if data: self.w(data)
        self.central(e, h, method, crc, len(data), usize)
        self.stats['deflated' if method == 8 else 'stored'] += usize; self.stats['out'] += len(data)

    def other(self, e):   # a folder or a symlink
        if e.st and stat.S_ISLNK(e.st.st_mode):
            t = os.readlink(e.path).encode('utf-8', 'surrogateescape'); self.put(e, 0, zlib.crc32(t), t, len(t))
        else: self.put(e, 0, 0, b'', 0)

    def stream(self, e, deflate):   # a big file: the local header first, its CRC (and a deflated size) patched in afterwards
        usize = e.st.st_size; z64 = usize >= L32   # a deflate that is not smaller is redone stored, so the deflated size stays below usize
        h = self.local(e, 8 if deflate else 0, 0, 0 if deflate else usize, usize, z64)
        crc, csize, c = 0, 0, zlib.compressobj(6, zlib.DEFLATED, -15) if deflate else None
        with open(e.path, 'rb') as f:
            while b := f.read(CHUNK):
                crc = zlib.crc32(b, crc)
                if c: b = c.compress(b)
                self.w(b); csize += len(b)
            if c: b = c.flush(); self.w(b); csize += len(b)
        if csize != usize and not c: sys.exit(f'[RED] pack_ipa: {e.path} changed size while it was packed')
        if c and csize >= usize:   # not smaller: stored instead
            self.f.seek(h[0]); self.f.truncate(); self.pos = h[0]; return self.stream(e, False)
        self.f.seek(h[0] + 14); self.f.write(struct.pack('<I', crc) + (b'' if z64 else struct.pack('<II', csize, usize)))
        if z64: self.f.seek(h[0] + 30 + len(h[1]) + 4); self.f.write(struct.pack('<QQ', usize, csize))
        self.f.seek(self.pos)
        self.central(e, h, 8 if c else 0, crc, csize, usize)
        self.stats['deflated' if c else 'stored'] += usize; self.stats['out'] += csize

    def close(self):   # the central directory and the end records (zip64 ones when the count, the size or the offset does not fit)
        cd_off, n = self.pos, len(self.cd); blob = b''.join(self.cd); self.w(blob)
        if n >= L16 or cd_off >= L32 or len(blob) >= L32:
            z = self.pos
            self.w(struct.pack('<IQHHIIQQQQ', 0x06064B50, 44, 0x031E, 45, 0, 0, n, n, len(blob), cd_off))
            self.w(struct.pack('<IIQI', 0x07064B50, 0, z, 1))
        self.w(struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, min(n, L16), min(n, L16), min(len(blob), L32), min(cd_off, L32), 0))
        self.f.close()


def main():
    ap = argparse.ArgumentParser(description='Pack an IPA (zip) from source folders mapped to archive paths, in parallel')
    ap.add_argument('out'); ap.add_argument('maps', nargs='+', metavar='archive_path=folder')
    ap.add_argument('--store', default='', help='suffixes stored without compressing, separated by ":"')
    ap.add_argument('--skip', action='append', default=[], help='an archive path left out of the folder walk that would add it')
    a = ap.parse_args()
    maps = []
    for m in a.maps:
        arc, sep, src = m.partition('=')
        if not sep or not arc.strip('/') or not os.path.isdir(src): sys.exit(f'[RED] pack_ipa: not <archive path>=<folder>: {m}')
        maps.append((arc.strip('/'), src))
    store = tuple(s.lower() for s in a.store.split(':') if s)
    t0 = time.time()
    es = entries(maps, {s.strip('/') for s in a.skip})
    total = sum(e.st.st_size for e in es if e.st and stat.S_ISREG(e.st.st_mode))
    print(f'pack: {len(es)} entries, {total} bytes', flush=True)
    part = a.out + '.partial'
    z = Zip(part)
    try:
        pend, inflight = collections.deque(), 0
        def one():   # write the oldest pending entry
            e, fut = pend.popleft()
            if fut is None: z.other(e); return 0
            method, crc, data = fut.result(); z.put(e, method, crc, data, e.st.st_size); return e.st.st_size
        with cf.ThreadPoolExecutor(os.cpu_count()) as pool:
            for e in es:
                if not (e.st and stat.S_ISREG(e.st.st_mode)): pend.append((e, None)); continue   # folders and symlinks keep their place in the queue
                deflate = not e.name.lower().endswith(store) if store else True
                if e.st.st_size > BIG:
                    while pend: inflight -= one()
                    z.stream(e, deflate); continue
                while pend and (len(pend) >= QUEUE or inflight + e.st.st_size > WINDOW): inflight -= one()
                pend.append((e, pool.submit(load, e.path, deflate))); inflight += e.st.st_size
            while pend: inflight -= one()
        z.close()
    except BaseException:
        z.f.close(); os.unlink(part); raise
    os.replace(part, a.out)
    s, dt = z.stats, time.time() - t0
    print(f'packed: {len(es)} entries, stored {s["stored"] / 2**30:.2f} GB, deflated {s["deflated"] / 2**30:.2f} GB, archive {os.path.getsize(a.out) / 2**30:.2f} GB '
          f'in {dt:.0f} s ({total / max(dt, 1e-3) / 2**20:.0f} MB/s)', flush=True)


if __name__ == '__main__': main()
