#!/usr/bin/env python3
# A game in a NUP container (one .nup file: https://github.com/a55uka/nup, SPEC.md v1) → the layout build_ipa_from_nsp.py extracts NSPs into. build_ipa_from_nsp.py calls this when it is given
# a .nup file, or a folder that holds one, and goes on with its other steps; run on its own, this script does only this step.
# A NUP holds the same packages as a dump folder (scripts/pack_dump.py): the base game, updates by version and DLC by title ID, each an exefs/ and romfs/ tree, its files stored raw or as one zstd
# frame each, with a SHA-256 per file. Its files are not encrypted, so it needs no keys. Unlike a dump it may carry control data in each package's meta block: the NACP and the icon. Its name,
# publisher and display version records are labels the packer typed (Hyrule Warriors: Age of Imprisonment's says "HWAoI"), not the console's: without a NACP the name comes from the profile.
# The base package may be an empty placeholder (no code, title ID 0) when an update holds the whole game (Pokemon Pokopia's v655360): the title ID is the program ID of the code present.
# Writes, under the build folder (<folder of the .nup>/<title ID>_build unless --work-dir names another):
#   extract/v<version>/ExeFS/   the version's code modules
#   extract/v<version>/RomFS.img   its RomFS as one image (dm/tools/romfs_image.py), written straight from the container: the RomFS is never unpacked into loose files
#   extract/v<version>/control.nacp, icon.jpg   the package's NACP and its icon, when it carries them (sips reads a PNG icon by its content)
#   aoc/<index>.img   each DLC's RomFS (index = DLC title ID − base title ID − 0x1000), the host's DM_AOC_DIR
# An update's RomFS is packed on its own, never laid over the base: the dumps NUPs are packed from hold the patched RomFS of an update (pack_dump.py); an update with no romfs/ uses the base's.
# Every file read is checked against its SHA-256. A repeat run keeps code modules whose content is the same and an image made from the same container tree by the same packer.
# Needs the zstd module: compression.zstd (Python 3.14+) or backports.zstd, as the NSP script.
# Usage: pack_nup.py <game.nup> [--version N] [--work-dir DIR]   (exit 1 on error)
import argparse, hashlib, mmap, os, shutil, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import romfs_image
try: from compression import zstd
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]  # an optional package, only Python before 3.14 needs it
    except ImportError: zstd = None


class Error(Exception): pass


SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')   # 0x80
TOC = struct.Struct('<4sIQQQQQ')   # 0x30
PKG = struct.Struct('<QIIIIBBHIQI20x')   # 0x40
DIR = struct.Struct('<IIIIQ')   # 0x18
FILE = struct.Struct('<IIQQQ32sBBH12x')   # 0x50
TLV = struct.Struct('<HHI')
NO_DIR = 0xFFFFFFFF
BASE, UPDATE, ADDON = 0, 1, 2
KINDS = ('base', 'update', 'addon', 'addon_update', 'system')
STANDALONE = 1   # package flags bit 0: an update in a container without a base
T_NACP, T_ICON, T_LICONS = 2, 3, 10
ARTIFACTS = {b'.listing'}   # dump-tool files a producer may have kept (SPEC §6.3): not game files


class Package:
    def __init__(self, nup, index, rec):
        self.nup, self.index = nup, index
        self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags, r1, r2, self.meta_off, self.meta_size = rec
        if self.kind >= len(KINDS): raise Error(f'{nup.path}: package {index} of unknown kind {self.kind}')
        if r1 or r2 or self.flags & ~STANDALONE: raise Error(f'{nup.path}: package {index} has reserved fields or flags set')
        self.name = f'{nup.path.name} ({KINDS[self.kind]}' + (f' {self.version}' if self.kind == UPDATE else f' {self.title_id:016X}' if self.kind == ADDON else '') + ')'   # what pick_nsps prints

    def meta(self):   # {TLV type: payload}
        out, b = {}, self.nup.mm[self.meta_off:self.meta_off + self.meta_size] if self.meta_size else b''
        p = 0
        while p + TLV.size <= len(b):
            t, _, n = TLV.unpack_from(b, p); p += TLV.size
            if p + n > len(b): raise Error(f'{self.nup.path}: package {self.index}: meta record {t:#x} runs past its block')
            out.setdefault(t, bytes(b[p:p + n])); p += n
        return out


class Nup:
    """A validated NUP (SPEC §6.1): the superblock, the TOC's SHA-256, every record's bounds. File content is checked against its SHA-256 as it is read"""
    def __init__(self, path):
        self.path = Path(path)
        with open(self.path, 'rb') as f:   # the map keeps its own descriptor
            try: self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            except ValueError: raise Error(f'{self.path}: empty file') from None
        try: self._read()
        except BaseException:
            self.close(); raise

    def _read(self):
        if len(self.mm) < SUPER.size: raise Error(f'{self.path}: not a NUP (too short)')
        magic, hsize, _, toc_off, toc_size, toc_sha, self.blob, fsize, npkg, flags, self.uuid, _ = SUPER.unpack_from(self.mm)
        if magic[:3] != b'NUP': raise Error(f'{self.path}: not a NUP')
        if magic[3] != 1: raise Error(f'{self.path}: NUP format version {magic[3]}; this reads version 1')
        if hsize < SUPER.size or toc_off != hsize or flags: raise Error(f'{self.path}: unsupported NUP header')
        if fsize != len(self.mm): raise Error(f'{self.path}: {len(self.mm):,} bytes, its header says {fsize:,} (copied or downloaded partly?)')
        if self.blob < toc_off + toc_size or self.blob % 16 or self.blob > fsize: raise Error(f'{self.path}: TOC and data regions overlap')
        toc = self.mm[toc_off:toc_off + toc_size]
        self.toc_sha = toc_sha
        if hashlib.sha256(toc).digest() != toc_sha: raise Error(f'{self.path}: table of contents does not match its SHA-256 (damaged file)')
        tmagic, tpkg, self.ndirs, nfiles, pool_off, pool_size, _ = TOC.unpack_from(toc)
        need = TOC.size + tpkg * PKG.size + self.ndirs * DIR.size + nfiles * FILE.size
        if tmagic != b'TOC\x01' or tpkg != npkg or not npkg or len(toc) < need or pool_off < need or pool_off + pool_size > len(toc) or (pool_off - need) % 4:
            raise Error(f'{self.path}: malformed table of contents')
        pool = toc[pool_off:pool_off + pool_size]
        def name(off, n):
            if off + n > len(pool): raise Error(f'{self.path}: name out of bounds')
            return pool[off:off + n]
        p = TOC.size
        self.packages = [Package(self, i, PKG.unpack_from(toc, p + i * PKG.size)) for i in range(npkg)]; p += npkg * PKG.size
        self.dirs = []   # (name, first child slot, child count)
        for _ in range(self.ndirs):
            o, n, first, count, _ = DIR.unpack_from(toc, p); p += DIR.size
            self.dirs.append((name(o, n), first, count))
        self.files = []   # (name, size, sha256, extent offset, extent size, codec)
        for _ in range(nfiles):
            o, n, size, eoff, esz, sha, codec, cflags, _ = FILE.unpack_from(toc, p); p += FILE.size
            if codec not in (0, 1) or cflags: raise Error(f'{self.path}: file {name(o, n)!r} uses codec {codec}; this reads raw and zstd')
            if size and (eoff < self.blob or eoff + esz > fsize or eoff % 16 or not esz or codec == 0 and esz != size): raise Error(f'{self.path}: file {name(o, n)!r} out of bounds')
            if not size and (eoff or esz or codec): raise Error(f'{self.path}: empty file {name(o, n)!r} with data')
            self.files.append((name(o, n), size, sha, eoff, esz, codec))
        self.slots = struct.unpack_from(f'<{(pool_off - p) // 4}I', toc, p)
        for n, first, count in self.dirs:
            if first + count > len(self.slots): raise Error(f'{self.path}: folder {n!r} out of bounds')
        for s in self.slots:
            if s >= self.ndirs + nfiles: raise Error(f'{self.path}: child {s} out of bounds')
        for k in self.packages:
            if k.root >= self.ndirs or any(d != NO_DIR and d >= self.ndirs for d in (k.exefs, k.romfs)) or k.meta_size and k.meta_off + k.meta_size > fsize:
                raise Error(f'{self.path}: package {k.index} out of bounds')

    def close(self):
        self.mm.close()

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def children(self, d):   # [(name, is dir, index into dirs or files)], sorted by name (bytes), artifacts left out
        _, first, count = self.dirs[d]
        out = []
        for s in self.slots[first:first + count]:
            if s < self.ndirs: out.append((self.dirs[s][0], True, s))
            elif self.files[s - self.ndirs][0] not in ARTIFACTS: out.append((self.files[s - self.ndirs][0], False, s - self.ndirs))
        return sorted(out)

    def child(self, d, name):   # the index of a file in folder d, or None
        return next((i for n, isdir, i in self.children(d) if n == name and not isdir), None)

    def tree_bytes(self, d):   # the uncompressed size of everything under folder d
        return sum(self.files[i][1] if not isdir else self.tree_bytes(i) for _, isdir, i in self.children(d))

    def copy(self, i, out):
        """File i's content to out (anything with write), streamed and checked against its size and SHA-256; returns its size"""
        name, size, sha, eoff, esz, codec = self.files[i]
        h, n, view = hashlib.sha256(), 0, memoryview(self.mm)
        def put(b):
            nonlocal n
            h.update(b); out.write(b); n += len(b)
        if codec == 0:
            for p in range(eoff, eoff + esz, 8 << 20): put(view[p:min(p + (8 << 20), eoff + esz)])
        elif size:
            if zstd is None: raise Error('reading a NUP needs the zstd module: compression.zstd (Python 3.14+) or pip install backports.zstd')
            d = zstd.ZstdDecompressor()
            try:
                for p in range(eoff, eoff + esz, 4 << 20):
                    put(d.decompress(view[p:min(p + (4 << 20), eoff + esz)]))
                    if n > size: break
            except zstd.ZstdError as e: raise Error(f'{self.path}: {name.decode(errors="replace")}: {e}') from e
            if not d.eof: n = -1
        view.release()
        if n != size or h.digest() != sha: raise Error(f'{self.path}: {name.decode(errors="replace")} does not match its size or SHA-256 (damaged file)')
        return n

    def read(self, i):
        class Buf(bytearray):
            write = bytearray.extend
        b = Buf(); self.copy(i, b); return bytes(b)


def program_id(npdm, where):   # the ACI0 program ID of an NPDM
    if npdm[:4] != b'META': raise Error(f'{where}: not an NPDM')
    off, size = struct.unpack_from('<II', npdm, 0x70)
    if npdm[off:off + 4] != b'ACI0' or size < 0x18: raise Error(f'{where}: no ACI0 section')
    return struct.unpack_from('<Q', npdm, off + 0x10)[0]


def is_nup(path):
    try:
        with open(path, 'rb') as f: return f.read(3) == b'NUP'
    except OSError: return False


def scan(n):
    """What a NUP holds: (title ID, base package, {update version: package}, {DLC title ID: package}), with the checks pack_dump.scan makes"""
    bases = [k for k in n.packages if k.kind == BASE]
    if not bases:
        raise Error(f'{n.path}: no base game, only ' + (', '.join(sorted({KINDS[k.kind] for k in n.packages}))) + ' packages: pack the base game and its update into one NUP')
    base = bases[0]
    if len(bases) > 1: raise Error(f'{n.path}: {len(bases)} base packages')
    # Dumps can leave an empty base placeholder and put the complete game in an update.
    # Identify it from the code actually present; each selected version is checked by pack_version.
    ids = {program_id(n.read(i), f'{k.name} exefs/main.npdm') for k in n.packages if k.kind in (BASE, UPDATE) and k.exefs != NO_DIR
           if (i := n.child(k.exefs, b'main.npdm')) is not None}
    if not ids: raise Error(f'{n.path}: no exefs/main.npdm in the base or updates')
    if len(ids) != 1: raise Error(f'{n.path}: base and updates have different program IDs: ' + ', '.join(f'{pid:016X}' for pid in sorted(ids)))
    app = ids.pop()
    if app & 0xFFF: raise Error(f'{n.path}: base program ID {app:016X} is not a base title ID')
    vers, dlc = {}, {}
    for k in n.packages:
        if k.kind == UPDATE:
            if k.version in vers: raise Error(f'{n.path}: two updates of version {k.version}')
            if not k.version: raise Error(f'{n.path}: an update of version 0')
            vers[k.version] = k
        elif k.kind == ADDON:
            if not 0 < k.title_id - app - 0x1000 <= 2000: raise Error(f'{n.path}: {k.title_id:016X} is not a DLC of {app:016X}')
            if k.romfs == NO_DIR: raise Error(f'{n.path}: DLC {k.title_id:016X} has no romfs/')
            if k.title_id in dlc: raise Error(f'{n.path}: DLC {k.title_id:016X} more than once')
            dlc[k.title_id] = k
    return app, base, vers, dlc


def exefs(n, k, dst):   # the package's ExeFS into dst, kept as it is when every file there has the same content
    files = []
    for name, isdir, i in n.children(k.exefs):
        if isdir: continue
        # NUP names are labels (§6.1). Validate them before using them as host paths.
        filename = os.fsdecode(name)
        if filename in ('', '.', '..') or '\0' in filename or Path(filename).name != filename:
            raise Error(f'{n.path}: invalid ExeFS name {name!r}')
        if not name.startswith(b'.'): files.append((name, i))
    def same():
        if not dst.is_dir() or {os.fsencode(p.name) for p in dst.iterdir()} != {name for name, _ in files}: return False
        def sha(f):
            with open(f, 'rb') as h: return hashlib.file_digest(h, 'sha256').digest()
        return all(sha(dst / os.fsdecode(name)) == n.files[i][2] for name, i in files)
    if same(): return
    tmp = dst.with_name(dst.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    for name, i in files:
        with open(tmp / os.fsdecode(name), 'wb') as f: n.copy(i, f)
    shutil.rmtree(dst, ignore_errors=True); tmp.rename(dst)


def entries(n, d):   # romfs_image.entries for a folder of the container: [name, file index, is dir, a, b], breadth-first
    ents, i = [[b'', d, True, 0, 0]], 0
    while i < len(ents):
        if ents[i][2]:
            kids = n.children(ents[i][1])
            ents[i][3], ents[i][4] = len(ents), len(kids)
            ents.extend([name, j, isdir, 0, 0] for name, isdir, j in kids)
        i += 1
    return ents


def pack(n, d, img, say=print):   # the RomFS folder d of the container → img, kept while the container tree and the packers are the same
    img.parent.mkdir(parents=True, exist_ok=True)
    ents, dst = entries(n, d), os.fspath(img)
    digest = hashlib.sha256(Path(romfs_image.__file__).read_bytes() + Path(__file__).read_bytes() + n.toc_sha + struct.pack('<I', d)).hexdigest()   # the TOC hash covers every name and file hash
    if romfs_image.reusable(dst, digest): return len(ents)
    say(f'packing {n.path.name} folder {d} → {img}')
    part = dst + '.part'
    try: romfs_image.publish(romfs_image.write(ents, dst, n.copy), dst, digest)
    except BaseException:
        Path(part).unlink(missing_ok=True); raise
    say(f'  {len(ents):,} entries, {img.stat().st_size / 2**30:.2f} GB')
    return len(ents)


def localized(payload):   # the localized icons record (TLV 0x000A) → {lowercase language tag: image bytes}
    out, p = {}, 0
    while p < len(payload):
        ln = payload[p]; tag = payload[p + 1:p + 1 + ln].decode(errors='replace').lower(); p += 1 + ln
        if p + 4 > len(payload): break
        dn = struct.unpack_from('<I', payload, p)[0]; p += 4
        out.setdefault(tag, payload[p:p + dn]); p += dn
    return out


def pick(tags, code):   # the record for a language code: the exact tag, then its primary subtag
    code = code.lower()
    return tags.get(code) or tags.get(code.split('-')[0])


def control(k, out):   # the package's control data beside its ExeFS, as nx_extract saves it: control.nacp and icon.jpg (none of either when the package has none)
    meta = k.meta()
    nacp_bytes = meta.get(T_NACP)
    icons = localized(meta.get(T_LICONS, b''))
    icon = meta.get(T_ICON) or pick(icons, 'en-US') or next(iter(icons.values()), None)
    for name, data in (('control.nacp', nacp_bytes), ('icon.jpg', icon)):
        if data: (out / name).write_bytes(data)
        else: (out / name).unlink(missing_ok=True)


def romfs_of(k, base):   # the RomFS folder a version runs with: its own, or the base game's for an update without one
    d = k.romfs if k.romfs != NO_DIR else base.romfs
    if d == NO_DIR: raise Error(f'{k.name}: no romfs/ in this package or the base game')
    return d


def pack_version(n, k, base, app, out, say=print):   # one version (the base package, or an update package) → out/ExeFS, out/RomFS.img and its control data
    if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None: raise Error(f'{k.name}: no exefs/main and exefs/main.npdm')
    if (pid := program_id(n.read(n.child(k.exefs, b'main.npdm')), f'{k.name} exefs/main.npdm')) != app: raise Error(f'{k.name}: program ID {pid:016X}, the base is {app:016X}')
    d = romfs_of(k, base)
    out.mkdir(parents=True, exist_ok=True)
    exefs(n, k, out / 'ExeFS')
    pack(n, d, out / 'RomFS.img', say)
    control(k, out)


def pack_dlc(n, app, dlc, aoc, say=print):   # {DLC title ID: package} → aoc/<index>.img; images of DLC no longer in the container go
    want = {f'{tid - app - 0x1000}.img': k for tid, k in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, k in want.items(): pack(n, k.romfs, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a game in a NUP container into the extraction layout of build_ipa_from_nsp.py')
    ap.add_argument('nup', type=Path, help='the .nup file')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in the container; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <folder of the .nup>/<title ID>_build)')
    a = ap.parse_args()
    path = a.nup.resolve()
    try:
        with Nup(path) as n:
            app, base, vers, dlc = scan(n)
            ver = a.version if a.version is not None else max(vers, default=0)
            if ver and ver not in vers: raise Error(f'no update {ver} in {path.name} (have: {", ".join(map(str, sorted(vers))) or "none"})')
            work = (a.work_dir or path.parent / f'{app:016X}_build').resolve()
            out = work / 'extract' / f'v{ver}'
            print(f'{app:016X} version {ver}: {(vers[ver] if ver else base).name}', flush=True)
            say = lambda s: print(s, flush=True)
            pack_version(n, vers[ver] if ver else base, base, app, out, say)
            pack_dlc(n, app, dlc, work / 'aoc', say)
    except (Error, OSError) as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
