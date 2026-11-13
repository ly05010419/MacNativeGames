#!/usr/bin/env python3
"""The shader seed's per-shader files as image parts beside the seed folder, <folder>.img.000, .001, ... (2026-11-13,
after nxnative's b-<chain>.img, dm/tools/shader_lib.py). The runtime reads a shader's file in place through the part's
index (games/totk/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h); an iPad used to unpack about 128,000 files at the
first start after an update and remove the last seed's 128,000 afterwards. Every game's copy of this file is the same.

Format (little-endian): a 64-byte header ("TOTKIMG1", u32 version 1, u32 count, u64 index offset, u64 names offset,
u64 names length, zeros), the files' bytes each at a multiple of ALIGN, the index (count entries of u64 offset, u64 size,
u32 name offset, u32 name length, sorted by name bytes), then the names. Each part is a whole image: no file spans two.
"""
import re
import struct
from pathlib import Path

MAGIC = b'TOTKIMG1'
VERSION = 1
HEADER = 64
ALIGN = 16
ENTRY = struct.Struct('<QQII')
# A shader's own files: <SHA-256 of its code>-<chain>_b.metal / .json / .metallib and <SHA-256>.variants. The seed's other
# files (seed-manifest.json, the lists, the device pipeline archives, variant-*.metallib) stay files in the folder.
SHADER_FILE = re.compile(r'^[0-9a-f]{64}[-.]')


def image_names(names):
    """The names among a seed folder's file names that go into its image."""
    return sorted(n for n in names if SHADER_FILE.match(n))


def _pad(f, at):
    gap = -at % ALIGN
    if gap: f.write(b'\0' * gap)
    return at + gap


def _finish(f, at, entries):
    """The index and names after the files, then the header: the part is whole."""
    at = _pad(f, at)
    index = at; names = bytearray(); table = bytearray()
    for name, off, size in sorted(entries, key=lambda e: e[0]):
        table += ENTRY.pack(off, size, len(names), len(name)); names += name
    f.write(table); f.write(names)
    f.seek(0)
    f.write(MAGIC + struct.pack('<IIQQQ', VERSION, len(entries), index, index + len(table), len(names)) + b'\0' * (HEADER - 40))


def write_parts(seed, names, base, part_size, on_part=None):
    """The files names of the folder seed into parts <base>.img.000, ... of at most part_size bytes each (a file larger
    than that gets a part of its own); on_part(path) is called as each part is complete. Returns the parts' paths."""
    seed, base = Path(seed), Path(base)
    names = sorted(names, key=lambda n: n.encode())
    parts = []; f = None; at = 0; entries = []

    def close():
        nonlocal f
        _finish(f, at, entries); f.close(); f = None
        if on_part: on_part(parts[-1])
    for name in names:
        size = (seed/name).stat().st_size
        if f is not None and entries and at + -at % ALIGN + size + (len(entries) + 1) * (ENTRY.size + 128) > part_size:
            close()
        if f is None:
            parts.append(base.with_name(f'{base.name}.img.{len(parts):03d}'))
            f = open(parts[-1], 'wb'); f.write(b'\0' * HEADER); at = HEADER; entries = []
        at = _pad(f, at)
        with open(seed/name, 'rb') as src:
            while chunk := src.read(1 << 20): f.write(chunk)
        entries.append((name.encode(), at, size)); at += size
    if f is not None: close()
    return parts


def read_part(path):
    """{name: bytes} of one part (tests and checks)."""
    data = Path(path).read_bytes()
    if data[:8] != MAGIC: raise ValueError(f'{path}: not a seed image')
    version, count, index, names, nlen = struct.unpack_from('<IIQQQ', data, 8)
    if version != VERSION or names != index + count * ENTRY.size or names + nlen != len(data): raise ValueError(f'{path}: damaged')
    out = {}
    for i in range(count):
        off, size, name, length = ENTRY.unpack_from(data, index + i * ENTRY.size)
        out[data[names + name:names + name + length].decode()] = data[off:off + size]
    return out
