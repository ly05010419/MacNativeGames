#!/usr/bin/env python3
"""Find every Switch 2 shader a game ships, for translating them before the game runs (`totk-native-<v> <modules>
seed-shaders <corpus>`; docs/ns2-port.md).

A Switch 2 game's shaders are GLSLC compiler outputs (magic 0x19866891, output version 0x00220015 at +8). Each holds
an NVN2 control section (0xe0 bytes, magic 0x98761234, version 0xc9; runtime nvn_ctl.h) followed by the code area the
game hands nvnProgramSetShaders; the output's header gives the header length (+0xdc) and the end of the control
section (+0x60, from the header's end). Pokemon Pokopia keeps them in its game modules' read-only data, in Koei Tecmo's
LINKDATA (LI_C0_R.IDX / LD_C0_R.BIN) and in the IDRK entries of its RDB data files (motor_asset_re/*/*.fdata,
data/*.file). Layout and container formats read from xFractals/pokopia-native tools/glslc2_extract.py and
seedds/nxnative dm/gfx/tools/shader_split.py (idrk_entries, idrk_data, kt_gz), both MIT.

The corpus keeps each shader once, by the runtime's shader key (nvn_ctl.h ctl_key_update: the control section's first
0x48 bytes with +0x30 as zero, then the code area, control +0x44 bytes long). It is game-derived data: it belongs in the
work folder beside the game, never in the repository.

  ns2_shader_corpus.py --romfs <RomFS folder, or a packed one with index.json + romfs.bin> --modules <ELF ...> --out corpus.bin
"""
import argparse
import concurrent.futures
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import struct
import sys
import zlib

GLSLC = b'\x91\x68\x86\x19'
VERSIONS = (0x00220015, 0x00220615)   # GLSLC output versions (+8); 0x220615: 3 of Pokopia's 213,169 objects
CTL_MAGIC = b'\x34\x12\x76\x98'
CTL_SIZE = 0xe0
IDRK = b'IDRK0000'
CORPUS_MAGIC = b'NS2SHC01'
SPH_STAGE = {0: 0, 1: 0, 2: 3, 3: 4, 4: 2, 5: 1}   # SPH shader type -> NVN stage (nvn_ctl.h ctl_stage)


def stage_of(ctl, code):
    """The NVN stage (0 vertex, 1 fragment, 2 geometry, 3 / 4 tessellation, 5 compute) as the runtime reads it, or None."""
    if ctl[0x56] == 5:
        return 5
    if len(code) < 4:
        return None
    w0 = struct.unpack_from('<I', code, 0)[0]
    kind = (w0 >> 10) & 0xf
    return SPH_STAGE.get(kind) if w0 & 0x1f else None


def shader_key(ctl, code):
    k = bytearray(ctl[:0x48])
    k[0x30:0x34] = bytes(4)
    return hashlib.sha256(bytes(k) + code).hexdigest()


def containers(d, label):
    """(key, stage, control, code, label) of every GLSLC output in d that passes the checks. The output's header gives
    its size (+0x5c) and where its control section starts (+0x60); the control section gives its own size (+0x30) and the
    code area's length (+0x44, the runtime's ctl_code_len), and the code area follows it. Until 2026-11-04 the control
    section was found from other header fields and taken as 0xe0 bytes long: that held for 2,204 of the 130,904 shaders in
    Pokemon Pokopia's RDB data and missed the rest (kirakira-dev/ouncenative nx2_shader_extract.py, MIT, reads the layout
    this way), so most shaders the game sets were translated in play."""
    out = []
    for m in re.finditer(GLSLC, d):
        h = m.start()
        if h + 0x68 > len(d) or struct.unpack_from('<I', d, h + 8)[0] not in VERSIONS:
            continue
        total, at = struct.unpack_from('<II', d, h + 0x5c)
        c = h + at
        if not (0x60 <= at and at + 0x48 <= total <= len(d) - h) or d[c:c + 4] != CTL_MAGIC or d[c + 4:c + 8] != b'\xc9\0\0\0':
            continue
        size, n = struct.unpack_from('<I', d, c + 0x30)[0], struct.unpack_from('<I', d, c + 0x44)[0]
        if not (0xd8 <= size < 0x10000 and 0 < n < 0x400000 and at + size + n <= total):   # Pokopia's are 0xd8 long (the runtime reads 0xe0: the rest is the code's start, as in the game's memory)
            continue
        ctl = bytes(d[c:c + CTL_SIZE])
        code = bytes(d[c + size:c + size + n])
        st = stage_of(ctl, code)
        if st is None:
            continue
        out.append((shader_key(ctl, code), st, ctl, code, f'{label}@{h:#x}'))
    return out


def kt_gz(d):
    """Koei Tecmo's chunked zlib (nxnative shader_split.py kt_gz): the inflated data, or None when d is not in that form."""
    if len(d) < 16:
        return None
    cs, n, tot = struct.unpack_from('<III', d)
    if cs != 0x10000 or not n or n != (tot + cs - 1) // cs or 12 + 4 * n > len(d):
        return None
    sizes = struct.unpack_from(f'<{n}I', d, 12)
    o, out = (12 + 4 * n + 0x7f) & ~0x7f, []
    for i, s in enumerate(sizes):
        if o + s > len(d):
            return None
        if s == min(cs, tot - i * cs):
            out.append(bytes(d[o:o + s]))
        else:
            ln = struct.unpack_from('<I', d, o)[0]
            try:
                out.append(zlib.decompress(d[o + 4:o + 4 + ln]))
            except zlib.error:
                return None
        o = (o + s + 0x7f) & ~0x7f
    return b''.join(out)


def idrk_entries(d):
    """(start of the stored data, its end, flags) of each IDRK entry (nxnative shader_split.py idrk_entries)."""
    o = 0
    while o + 0x30 <= len(d):
        if d[o:o + 8] != IDRK:
            o = d.find(IDRK, o)
            if o < 0:
                return
        esz, csz = unpack('<QQ', d, o + 8)
        if not esz or o + esz > len(d) or csz > esz:
            raise ValueError(f'IDRK entry at {o:#x}: lengths {esz:#x} / {csz:#x} past the file')
        yield o + esz - csz, o + esz, unpack('<I', d, o + 0x2c)[0]
        o += esz


def idrk_data(d, a, b, flags):
    """An IDRK entry's data (nxnative shader_split.py idrk_data): 0x400000 chunks of u16 stored length, 8 bytes, a stream
    (LZ4 with 0x100000 too, else zlib); 0x100000 alone chunks of u32 length + zlib; anything else stored."""
    if not flags & 0x500000:
        return d[a:b]
    new, lz4b, out = flags & 0x400000, flags & 0x500000 == 0x500000, []
    if lz4b:
        import lz4.block
    while a + 4 <= b:
        if new:
            cs = unpack('<H', d, a)[0]
            a += 10
        else:
            cs = unpack('<I', d, a)[0]
            a += 4
        if not cs:
            break
        out.append(lz4.block.decompress(d[a:a + cs], uncompressed_size=0x4000) if lz4b else zlib.decompress(d[a:a + cs]))
        a += cs
    return b''.join(out)


class Window:
    """One file inside a mapped packed RomFS: bytes-like for slicing, find and struct reads, in the file's own offsets."""

    def __init__(self, m, base, size):
        self.m, self.base, self.size = m, base, size

    def __len__(self):
        return self.size

    def __getitem__(self, i):
        if not isinstance(i, slice):
            raise TypeError('Window takes slices')
        a, b, _ = i.indices(self.size)
        return self.m[self.base + a:self.base + max(a, b)]

    def find(self, sub, start=0, end=None):
        end = self.size if end is None else min(end, self.size)
        r = self.m.find(sub, self.base + start, self.base + end)
        return r - self.base if r >= 0 else -1


def unpack(fmt, d, o):
    return struct.unpack_from(fmt, d.m, d.base + o) if isinstance(d, Window) else struct.unpack_from(fmt, d, o)


class Source:
    """A file of the RomFS, from a folder or from a packed RomFS (index.json + romfs.bin, games/totk/scripts/import_dump.py)."""

    def __init__(self, root):
        self.root = Path(root)
        index = self.root/'index.json'
        self.packed = index.is_file()
        self.files = ({f['path']: (f['offset'], f['size']) for f in json.loads(index.read_text())['files']} if self.packed
                      else {str(p.relative_to(self.root)): None for p in self.root.rglob('*') if p.is_file()})

    def open(self, path):
        """The file's bytes as a buffer (mapped)."""
        if not self.packed:
            f = open(self.root/path, 'rb')
            return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        offset, size = self.files[path]
        if not hasattr(self, 'image'):
            f = open(self.root/'romfs.bin', 'rb')
            self.image = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        if offset + size > len(self.image):
            raise ValueError(f'{path}: past the end of romfs.bin')
        return Window(self.image, offset, size)


def scan_rdb(root, path):
    src, out, entries = Source(root), [], 0
    d = src.open(path)
    for a, b, flags in idrk_entries(d):
        entries += 1
        data = idrk_data(d, a, b, flags)
        if GLSLC in data:
            out += containers(data, f'{path}#IDRK@{a:#x}')
    return path, entries, out


def scan_linkdata(root, idx_path, bin_path):
    src, out = Source(root), []
    idx, d = src.open(idx_path)[:], src.open(bin_path)
    n = len(idx) // 32
    for i in range(n):
        off, inflated, stored, comp = struct.unpack_from('<QQQQ', idx, i * 32)
        if not inflated:
            continue
        raw = bytes(d[off:off + (stored if comp else inflated)])
        data = (kt_gz(raw) if comp else None) or raw
        if GLSLC in data:
            out += containers(data, f'linkdata#{i}')
    return bin_path, n, out


def scan_module(path):
    return str(path), 1, nintendo_shaders(Path(path).read_bytes(), Path(path).stem)


def nintendo_shaders(data, label, depth=0):
    """Nintendo's NVN2 resources. Layouts: seedds/nxnative shader_split.py (MIT), BNSH, BAHS, NDSR and sead primitives.
    Bounds are checked before a record is accepted; keys are the host's full SHA-256, not nxnative's short names."""
    from prepare_shader_seed import sarc
    import compression.zstd as zstd
    if depth > 12:
        raise ValueError(f'{label}: archives nested too deeply')
    if data[:4] == b'\x28\xb5\x2f\xfd':
        data = zstd.decompress(data)
    if data[:4] == b'SARC':
        return [s for name, member in sarc(data).items()
                for s in nintendo_shaders(member, f'{label}/{name}', depth + 1)]
    out = containers(data, label)
    def add(c, nc, k, nk, where):
        if not (0 <= c <= len(data) - max(nc, 0x58) and 0 <= k <= len(data) - nk and 0 < nk < 0x400000):
            raise ValueError(f'{where}: shader outside resource')
        ctl = data[c:c + max(nc, CTL_SIZE)].ljust(CTL_SIZE, b'\0')
        if ctl[:8] != CTL_MAGIC + b'\xc9\0\0\0':
            return
        if struct.unpack_from('<I', ctl, 0x44)[0] != nk:
            raise ValueError(f'{where}: code length differs from control section')
        code = data[k:k + nk]
        st = stage_of(ctl, code)
        if st is None:
            return   # BAHS tables also contain records that are not shader programs
        out.append((shader_key(ctl, code), st, ctl[:CTL_SIZE], code, where))
    for match in re.finditer(b'BNSH\0\0\0\0', data):
        b = match.start()
        if b + 0x88 > len(data) or data[b + 0x60:b + 0x64] != b'grsc':
            continue
        size = struct.unpack_from('<I', data, b + 0x1c)[0]
        n, table = struct.unpack_from('<IQ', data, b + 0x7c)
        if not (0x88 <= size <= len(data) - b and table + n * 64 <= size):
            raise ValueError(f'{label}: invalid BNSH table')
        for i in range(n):
            prog = struct.unpack_from('<Q', data, b + table + i * 64 + 16)[0]
            if not prog:
                continue
            if prog + 56 > size:
                raise ValueError(f'{label}: invalid BNSH program')
            for si in range(6):
                sc = struct.unpack_from('<Q', data, b + prog + 8 + si * 8)[0]
                if not sc:
                    continue
                if sc + 32 > size:
                    raise ValueError(f'{label}: invalid BNSH stage')
                c, k, nk, nc = struct.unpack_from('<QQII', data, b + sc + 8)
                if c + nc > size or k + nk > size:
                    raise ValueError(f'{label}: invalid BNSH shader')
                add(b + c, nc, b + k, nk, f'{label}#BNSH@{b:#x}#{i}/{si}')
    if data[:4] == b'BAHS':
        rsz, region = struct.unpack_from('<II', data, 0x18)
        if region + rsz > len(data):
            raise ValueError(f'{label}: invalid BAHS code region')
        for a in range(0, region - 16, 4):
            off, zero, nc, rel = struct.unpack_from('<IIIi', data, a)
            c = a + rel
            if zero or off % 256 or not (0x58 <= nc < 0x10000 and 0 <= c <= region - nc):
                continue
            if data[c:c + 8] == CTL_MAGIC + b'\xc9\0\0\0':
                nk = struct.unpack_from('<I', data, c + 0x44)[0]
                add(c, nc, region + off, nk, f'{label}#BAHS@{a:#x}')
    if data[:4] == b'NDSR':
        def score(o):
            return sum(data[c:c + 8] == CTL_MAGIC + b'\xc9\0\0\0'
                       for c, _, _, _ in struct.iter_unpack('<4I', data[o:o + 256]))
        o = max((0x10, 0x14), key=score)
        while o + 16 <= len(data):
            c, cl, k, _ = struct.unpack_from('<4I', data, o); o += 16
            if not c and not cl:
                break
            if c + 0x48 <= len(data) and data[c:c + 8] == CTL_MAGIC + b'\xc9\0\0\0':
                add(c, cl >> 4, k, struct.unpack_from('<I', data, c + 0x44)[0], f'{label}#NDSR@{c:#x}')
    if 'primitive_drawer_nvn2_shader' in label and len(data) >= 16:
        vc, fc, vk, fk = struct.unpack_from('<4I', data)
        for c, k in ((vc, vk), (fc, fk)):
            add(c, struct.unpack_from('<I', data, c + 0x30)[0], k,
                struct.unpack_from('<I', data, c + 0x44)[0], f'{label}#sead@{c:#x}')
    return out


def scan_nintendo(root, path):
    return path, 1, nintendo_shaders(Source(root).open(path)[:], path)


def write_corpus(path, shaders):
    tmp = Path(str(path) + '.partial')
    with open(tmp, 'wb') as f:
        f.write(CORPUS_MAGIC + struct.pack('<I', len(shaders)))
        for key in sorted(shaders):
            st, ctl, code, label = shaders[key]
            lb = label.encode()[:0xffff]
            f.write(struct.pack('<IIIH', st, len(ctl), len(code), len(lb)) + lb + ctl + code)
    os.replace(tmp, path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', type=Path, required=True)
    p.add_argument('--modules', type=Path, nargs='+', required=True, help='the game modules as ELF files (extracted/<v>/elf/*.elf)')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    p.add_argument('--listed', type=Path, help="the game's shared shader list: of the RDB data files' shaders (130,904 in "
                   "Pokopia, most of them never loaded) only those its V rows name; the modules' and LINKDATA's all")
    a = p.parse_args()
    listed = None
    if a.listed:
        listed = {f[1] for f in (line.split('\t') for line in a.listed.read_text().splitlines()) if len(f) == 3 and f[0] == 'V'}
    src = Source(a.romfs)
    rdbs = sorted(f for f in src.files if f.endswith(('.fdata', '.file')))
    link = ('LI_C0_R.IDX', 'LD_C0_R.BIN') if {'LI_C0_R.IDX', 'LD_C0_R.BIN'} <= src.files.keys() else None
    nintendo = sorted(f for f in src.files if f.startswith(('Shader/', 'Lib/', 'Effect/', 'Layout/'))
                      and f.endswith(('.zs', '.bnsh', '_nvn2_shader.bin')))
    if not rdbs and not link and not nintendo:
        sys.exit(f'[RED] {a.romfs}: no RDB data files and no LINKDATA: not a Koei Tecmo RomFS this script reads')
    shaders, per_source, failed, startup = {}, {}, [], set()
    with concurrent.futures.ProcessPoolExecutor(a.jobs) as ex:
        jobs = [ex.submit(scan_module, m) for m in a.modules] + [ex.submit(scan_rdb, a.romfs, f) for f in rdbs]
        jobs += [ex.submit(scan_nintendo, a.romfs, f) for f in nintendo]
        if link:
            jobs.append(ex.submit(scan_linkdata, a.romfs, *link))
        for j in concurrent.futures.as_completed(jobs):
            try:
                name, units, found = j.result()
            except Exception as e:   # a file this script cannot read stops it: a corpus missing shaders would look complete
                failed.append(repr(e))
                continue
            per_source[name] = len(found)
            for key, st, ctl, code, label in found:
                shaders.setdefault(key, (st, ctl, code, label))
                if not name.endswith(('.fdata', '.file')):   # a module's or LINKDATA's: always in the corpus
                    startup.add(key)
            print(f'{name}: {units} entries, {len(found)} shaders', flush=True)
    if failed:
        sys.exit('[RED] ' + '\n'.join(failed))
    found = len(shaders)
    if listed is not None:
        shaders = {k: v for k, v in shaders.items() if k in startup or k in listed}
        print(f'{found} distinct shaders found; kept {len(shaders)}: {len(startup)} of the modules and LINKDATA and '
              f'{len(shaders) - len(startup & shaders.keys())} of the RDB data the list names ({len(listed)} listed)')
    by_stage = {}
    for st, *_ in shaders.values():
        by_stage[st] = by_stage.get(st, 0) + 1
    write_corpus(a.out, shaders)
    print(f'{len(shaders)} distinct shaders (by stage: {dict(sorted(by_stage.items()))}) -> {a.out}')


if __name__ == '__main__':
    main()
