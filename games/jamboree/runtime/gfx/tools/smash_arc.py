#!/usr/bin/env python3
"""Super Smash Bros. Ultimate's data.arc: the file-system table and the current file of each path (files), and a file's bytes (read). Used by shader_split.py (its shaders) and
dm/tools/nro_collect.py (its NROs). The paths are only Hash40 values (CRC-32 of the path and its length); the extension Hash40 (crc32(b'nro') | 3 << 32) tells a file's kind."""
import struct, sys

MAGIC = struct.pack('<Q', 0xABCDEF9876543210)
ZSTD_MAGIC = bytes.fromhex('28b52ffd')


def zstd_mod():   # compression.zstd from Python 3.14, its backport before (pip install backports.zstd), the same API
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: sys.exit(f'[RED] Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    return zstd


def files(path, regions=False):
    """Super Smash Bros. Ultimate's data.arc (17.6 GB at 13.0.5): u64 magic 0xABCDEF9876543210, then u64 offsets of the music section, the file section, the shared file section,
    the file system table and its search table. The table is {u32 data start (0x10), u32 size, u32 stored size, u32 section size} and one zstd frame. Inflated: a 0x58-byte header
    (u32 table size, path count, path-to-info index count, directory count, directory offset count, child hash count, file info count, info-to-data count, file data count,
    second directory offset count, second file data count, 0, 0x10, 0x10, u8 region count, u8 language count, u16, u32 version (0x000d0005 = 13.0.5), extra directory offsets,
    extra files, 0, extra files again, extra info-to-data, extra info-to-data sub count), the regions (12 bytes each), the stream header {quick directory count, stream hash count,
    stream file index count, stream offset count} and those tables (12, 8 + 12, 4 and 16 bytes each), {u32 path hash group count, u32 bucket count} with the buckets (8) and groups (8),
    then the paths (32 bytes: four Hash40 references {u32 CRC-32, u8 length, u24 index}: path, extension, parent, name), path-to-info indices (8: directory offset index, file info
    index), directory hashes (8) and directory infos (0x34), directory offsets (0x1c: u64 offset in the file section, sizes, first file, file count, directory index), child hashes (8),
    file infos (16: path index, path-to-info index, info-to-data index, flags), info-to-data (12: directory offset index, file data index, load type) and file data (16: offset in the
    directory / 4, stored size, size, flags; 3 = zstd). A file's data is at file section + its directory offset + 4 × its offset. The table ends with a section the lookups do not use.
    The update rewrote the archive in place and kept the superseded copies, which the file infos still reach (764 NROs for 98 paths); following each path to its file info gives the
    current file only. A regional file (file info flag 0x8000) has one info-to-data entry per region from its index on (the header's region count, 14): regions=True gives every
    region's data (the shader split: 128 of 13.0.5's effect shaders exist only in other regions' files), else the first.
    → [(path Hash40, absolute offset, stored size, size, extension Hash40)] in offset order, one per data, or None for another file"""
    with open(path, 'rb') as f:
        h = f.read(0x38)
        if h[:8] != MAGIC: return None
        file_off, fs_off = struct.unpack_from('<Q', h, 0x10)[0], struct.unpack_from('<Q', h, 0x20)[0]
        f.seek(fs_off); _, size, stored, _ = struct.unpack('<4I', f.read(16))
        fs = zstd_mod().decompress(f.read(stored))
    if len(fs) != size: sys.exit(f'[RED] {path}: file system table inflates to {len(fs):#x} bytes, its header says {size:#x}')
    (tsize, n_paths, n_idx, n_dirs, n_off1, n_hfold, n_info, n_sub, n_data, n_off2, n_data2, _) = struct.unpack_from('<12I', fs, 0)
    version, extra_dirs, extra_files, _, _, extra_f2d, _ = struct.unpack_from('<7I', fs, 0x3c)
    o = 0x58 + fs[0x38] * 12
    qd, sh, sfi, soe = struct.unpack_from('<4I', fs, o); o += 16 + qd * 12 + sh * 20 + sfi * 4 + soe * 16
    groups, buckets = struct.unpack_from('<2I', fs, o); o += 8 + buckets * 8 + groups * 8
    P = o; o += n_paths * 32
    IDX = o; o += n_idx * 8 + n_dirs * (8 + 0x34)
    DO = o; n_do = n_off1 + n_off2 + extra_dirs; o += n_do * 0x1c + n_hfold * 8
    FI = o; n_fi = n_info + n_data2 + extra_files; o += n_fi * 16
    F2D = o; n_f2d = n_sub + n_data2 + extra_f2d; o += n_f2d * 12
    FD = o; n_fd = n_data + n_data2 + extra_files; o += n_fd * 16
    if o > tsize: sys.exit(f'[RED] {path}: version {version:#x}: the file system table does not have the layout this reader knows (it would end at {o:#x} of {tsize:#x})')
    out, seen = [], set()
    for i in range(n_paths):
        c, w = struct.unpack_from('<II', fs, P + 32 * i); k = w >> 8
        if k >= n_idx: continue
        fii = struct.unpack_from('<I', fs, IDX + 8 * k + 4)[0]
        if fii >= n_fi: continue
        f2d0, flags = struct.unpack_from('<II', fs, FI + 16 * fii + 8)
        for f2di in range(f2d0, f2d0 + (fs[0x38] if regions and flags & 0x8000 else 1)):
            if f2di >= n_f2d: break
            doi, fdi = struct.unpack_from('<II', fs, F2D + 12 * f2di)
            if doi >= n_do or fdi >= n_fd: continue
            off_in, csz, usz = struct.unpack_from('<3I', fs, FD + 16 * fdi)
            at = file_off + struct.unpack_from('<Q', fs, DO + 0x1c * doi)[0] + 4 * off_in
            if at in seen or not csz: continue
            ec, ew = struct.unpack_from('<II', fs, P + 32 * i + 8)
            seen.add(at); out.append((c | (w & 0xff) << 32, at, csz, usz, ec | (ew & 0xff) << 32))
    return sorted(out, key=lambda e: e[1])


def hash40(s):   # a path's or extension's Hash40: CRC-32 and length
    import zlib
    return zlib.crc32(s.encode()) | len(s) << 32


def read(m, e):
    """A file's bytes (m: the archive's bytes or mmap; e: an entry of files()), inflated when stored as a zstd frame"""
    d = m[e[1]:e[1] + e[2]]
    return zstd_mod().decompress(d) if d[:4] == ZSTD_MAGIC else d[:e[3]]
