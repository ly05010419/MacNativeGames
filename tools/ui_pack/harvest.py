"""The harvest folder the game writes (harvest.tsv and <key>.png) and the runtime's texture key."""
import struct
from pathlib import Path
from typing import NamedTuple

from tools.ui_pack.errors import UiPackError

M64 = (1 << 64) - 1


# --- the runtime's key (pkg_hash in games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m) ---

def pkg_hash(data: bytes) -> int:
    """The runtime's content hash of a texture's guest bytes: four 64-bit lanes over 32-byte chunks, an FNV-style tail, a final mix."""
    n = len(data)
    a = [(0x9E3779B97F4A7C15 ^ n) & M64, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5]
    body = n - n % 32
    for words in struct.iter_unpack('<4Q', data[:body]):
        for k in range(4):
            x = ((a[k] ^ words[k]) * 0xff51afd7ed558ccd) & M64
            a[k] = x ^ (x >> 29)
    h = a[0] ^ ((a[1] * 3) & M64) ^ ((a[2] * 5) & M64) ^ ((a[3] * 7) & M64)
    for byte in data[body:]:
        h = ((h ^ byte) * 0x100000001b3) & M64
    h ^= h >> 33
    h = (h * 0xc4ceb9fe1a85ec53) & M64
    return h ^ (h >> 33)


def texpack_key(data: bytes, width: int, height: int, nvn_format: int) -> str:
    """The pack key (and file name without .png) of a texture: "<hash 16 hex>-<w>x<h>-<format 2 hex>"."""
    return '%016x-%dx%d-%02x' % (pkg_hash(data), width, height, nvn_format)


class Row(NamedTuple):
    key: str
    width: int
    height: int
    metal: int
    cls: str


def read_harvest(path):
    rows, seen = [], set()
    lines = Path(path).read_text().splitlines()
    head = lines[0].split('\t') if lines else []
    need = ('key', 'width', 'height', 'metal_format', 'class')
    if any(n not in head for n in need):
        raise UiPackError(f'{path}: the header must have the columns {", ".join(need)}')
    for number, line in enumerate(lines[1:], 2):
        if not line.strip():
            continue
        cells = dict(zip(head, line.split('\t')))
        try:
            row = Row(cells['key'], int(cells['width']), int(cells['height']), int(cells['metal_format']), cells['class'])
        except (KeyError, ValueError):
            raise UiPackError(f'{path}:{number}: cannot read this line') from None
        if row.cls not in ('mask', 'colour'):
            raise UiPackError(f'{path}:{number}: class must be mask or colour, not {row.cls}')
        if row.key not in seen:
            seen.add(row.key)
            rows.append(row)
    return rows
