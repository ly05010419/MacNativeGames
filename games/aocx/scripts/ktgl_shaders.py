#!/usr/bin/env python3
"""Hyrule Warriors: Age of Calamity's NVN shaders, read from its own files: Koei Tecmo's RDB data files and main.

KTGL keeps its programs as GLSLC compiler outputs: inside G2S containers ('_S2G3000', a material's variations) and one
'_DOK0000' container (the engine's system resources) among the IDRK entries of the RDB data files (asset/*.rdb.bin*,
asset/data/*.file), and 118 more in main. An IDRK entry is 'IDRK0000', u64 entry length, u64 stored length, u64 length,
..., u32 flags at +0x2c, with the stored data at the end of the entry; flag 0x100000 means zlib in chunks (u32 stored
length + one zlib stream each, a zero length ends). These facts come from games/aoc's notes (nxnative's shader_split.py
was read for them; none of its code is used here).

A GLSLC output starts with 0x19866891; +0x44 is its length, +0x4c its section count, and section i's header is at
+0x90 + 0x90 i: {u32 size, u32 data offset (from the output), u32 type (0 = GPU code)}; a GPU code section's header adds
+0x30 control offset, +0x34 code offset (from the section's data), +0x38 code length, +0x3c control length. The control
section starts with 0x98761234; +0x700 is the start of the c1 constants in the code, +0x704 the code's length, +0x714
the stage (0 vertex, 1 fragment, 5 compute). Each shader is keyed as the runtime keys it (nvn_mtl.m prog_get): the
SHA-256 of the control section's first ctl_key_len bytes and the code's first +0x704 bytes.

Yields (key, stage, control, code); nothing derived from the game is written by this module.

  python3 scripts/ktgl_shaders.py <work>/extracted/local/romfs [--main <work>/rewritten/local/main.patched.bin]   (a census)
"""
import argparse
import concurrent.futures
import hashlib
import json
import mmap
import os
import struct
import sys
import zlib
from pathlib import Path

IDRK = b'IDRK0000'
GLSLC = struct.pack('<I', 0x19866891)
CONTROL_MAGIC = struct.pack('<I', 0x98761234)
CODE_MAGIC = struct.pack('<I', 0x12345678)
CONTAINERS = (b'_S2G', b'_DOK')
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}   # NVN stages at control+0x714 (the translator's three)
CHUNKED = 0x100000
SHADER_FILES = ('.rdb.bin', '.file')   # asset/*.rdb.bin, .bin1 ... and asset/data/*.file


class Damaged(ValueError):
    pass


def ctl_key_len(control):
    """How many bytes of a control section the runtime hashes (nvn_mtl.m ctl_key_len)."""
    size = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return size if 0x7cc <= size < 0x880 else 0x880


def shader(control, code):
    """(key, stage, control, code) as the runtime keys and the seed tool takes it, or None when the pair is not a shader
    the translator takes (a magic can occur in other data)."""
    if len(control) < 0x760 or control[:4] != CONTROL_MAGIC:
        return None
    key_len = ctl_key_len(control)
    if key_len > len(control):
        return None
    control = control[:key_len]
    c1, length = struct.unpack_from('<II', control, 0x700)
    stage = STAGES.get(struct.unpack_from('<I', control, 0x714)[0])
    if not stage or not 0 < length <= len(code) or c1 > length:
        return None
    if stage != 'cs' and (code[:4] != CODE_MAGIC or c1 < 0x30):
        return None
    code = code[:length]
    return hashlib.sha256(control + code).hexdigest(), stage, control, code


def glslc_outputs(data):
    """Every (control, code) of the GLSLC outputs in data, by their section headers."""
    at = data.find(GLSLC)
    while at >= 0:
        if at + 0x90 <= len(data):
            size, count = struct.unpack_from('<I', data, at + 0x44)[0], struct.unpack_from('<I', data, at + 0x4c)[0]
            if 0x90 < size <= len(data) - at and 0 < count <= 16 and 0x90 * (count + 1) <= size:
                for i in range(count):
                    head = at + 0x90 + 0x90 * i
                    sec_size, sec_at, kind = struct.unpack_from('<3I', data, head)
                    if kind or sec_at + sec_size > size:
                        continue
                    ctl_at, code_at, code_len, ctl_len = struct.unpack_from('<4I', data, head + 0x30)
                    if ctl_at + ctl_len > sec_size or code_at + code_len > sec_size:
                        continue
                    base = at + sec_at
                    yield data[base + ctl_at:base + ctl_at + ctl_len], data[base + code_at:base + code_at + code_len]
        at = data.find(GLSLC, at + 4)


def shaders_in(data):
    for control, code in glslc_outputs(data):
        found = shader(control, code)
        if found:
            yield found


def idrk_entries(d, lo=0, hi=None):
    """(start of the stored data, its end, flags) of each IDRK entry of an RDB data file at d[lo:hi]."""
    hi = len(d) if hi is None else hi
    at = lo
    while at + 0x30 <= hi:
        if d[at:at + 8] != IDRK:
            at = d.find(IDRK, at, hi)
            if at < 0:
                return
        size, stored = struct.unpack_from('<QQ', d, at + 8)
        if size < 0x30 or stored > size or at + size > hi:
            raise Damaged(f'IDRK entry at {at - lo:#x}: length {size:#x}, stored {stored:#x}')
        yield at + size - stored, at + size, struct.unpack_from('<I', d, at + 0x2c)[0]
        at += size


def entry_data(d, a, b, flags, head=None):
    """An entry's data: zlib in chunks (flag 0x100000) or stored; head: only its first bytes."""
    if not flags & CHUNKED:
        return bytes(d[a:min(b, a + head) if head else b])
    out = []
    while a + 4 <= b:
        n = struct.unpack_from('<I', d, a)[0]
        a += 4
        if not n:
            break
        if a + n > b:
            raise Damaged(f'zlib chunk at {a:#x} past its entry')
        if head:
            return zlib.decompressobj().decompress(bytes(d[a:a + min(n, 1024)]), head)
        out.append(zlib.decompress(d[a:a + n]))
        a += n
    return b''.join(out)


def file_shaders(job):
    """The shaders of one RDB data file inside the RomFS image: (path, [(key, stage, control, code)], containers)."""
    image, path, offset, size = job
    found, containers = [], 0
    with open(image, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
        for a, b, flags in idrk_entries(d, offset, offset + size):
            if entry_data(d, a, b, flags, 4) in CONTAINERS:
                containers += 1
                found.extend(shaders_in(entry_data(d, a, b, flags)))
    return path, found, containers


def romfs_jobs(romfs):
    index = json.loads((Path(romfs) / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    image = str(Path(romfs) / 'romfs.bin')
    return index, [(image, r['path'], r['offset'], r['size']) for r in index['files']
                   if r['path'].startswith('asset/') and (r['path'].endswith(SHADER_FILES) or '.rdb.bin' in r['path'])]


def inventory(romfs, main=None, workers=None):
    """{key: (stage, control, code, source)} of every shader in the RDB data files and main, and the files read."""
    index, jobs = romfs_jobs(romfs)
    shaders, containers, sources = {}, 0, []
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers or os.cpu_count()) as pool:
        for path, found, n in pool.map(file_shaders, sorted(jobs, key=lambda j: -j[3])):
            containers += n
            if found:
                sources.append(path)
            for key, stage, control, code in found:
                shaders.setdefault(key, (stage, control, code, path))
    archived = len(shaders)
    if main:
        for key, stage, control, code in shaders_in(Path(main).read_bytes()):
            shaders.setdefault(key, (stage, control, code, 'module:main'))
    return dict(shaders=shaders, containers=containers, archived=archived, files=len(jobs), sources=sorted(sources))


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('romfs', type=Path)
    p.add_argument('--main', type=Path)
    a = p.parse_args()
    inv = inventory(a.romfs, a.main)
    stages = {}
    for stage, *_ in inv['shaders'].values():
        stages[stage] = stages.get(stage, 0) + 1
    print(json.dumps(dict(shaders=len(inv['shaders']), in_rdb=inv['archived'], containers=inv['containers'],
                          files_read=inv['files'], files_with_shaders=inv['sources'], stages=stages), indent=1))


if __name__ == '__main__':
    sys.exit(main())
