#!/usr/bin/env python3
"""The one loader of Switch code modules: an NSO (ExeFS) or an NRO (loaded at run time with nn::ro) → its memory image of text + ro + data, bss not included.
Every tool that reads a module's code goes through load(). It accepts what Horizon accepts and checks it the same way, before anything the header claims is allocated; a damaged or
truncated file raises Error (a ValueError):
  · NSO, as Horizon's loader (Atmosphere's ldr_process_creation.cpp, ValidateNsoHeaders, LoadAutoLoadModuleSegment, CheckSegmentHash): memory offsets on pages, text at 0, then ro,
    then data, none overlapping (a gap is allowed); sizes below 2 GiB; an uncompressed segment is read at its size, a compressed one is no larger stored than loaded and must
    decompress to exactly its size (a short LZ4 stream decompresses without an error, and the image came out shorter or with the later segments shifted); a segment whose
    check-hash flag is set (every retail module: 93 of 93 surveyed, all 279 hashes matching) must match its SHA-256 in the header, so damaged bytes of the right size are caught too.
    A module with the flags cleared (a modified main) is not checked, as on the Switch.
  · NRO, as nn::ro (Atmosphere's ro_service_impl.cpp, ValidateNro): sizes in whole pages, text at 0, ro and data right after, data ending at the module's size (40 of 40 surveyed).
  · Beyond Horizon, so a small file cannot claim gigabytes: a compressed segment can be no more than LZ4 produces from its stored bytes (255 bytes per byte; zstd has no such
    bound, so a ZBIC segment is held only to its own size), and the image, gaps included, no more than that of the whole file.
  · Compression: LZ4, and ZBIC (NSO flag bit 7, system 22.0.0 on: Nintendo's zstd variant, its own magic and entropy-table coding, which stock zstd cannot read) through the zbic
    package, as lz4.block is used. A compressed NRO (system 23.0.0+) is an Error naming it, never a wrong image.
bss is only a size: the image never includes it (patch_nso.py lays it out; this script's output file gets it as a sparse tail).
Usage on its own: nso.py <module> <out.bin> (the whole memory image, bss included, for nso_fnmap.py and analysis)"""
import hashlib, struct, sys
from typing import NamedTuple

PAGE = 0x1000
LZ4_MAX_RATIO = 255   # an LZ4 block produces at most 255 bytes per stored byte (a match length extension byte)
SIZE_MAX = 0x7FFFFFFF   # Horizon reads segment sizes as s32
NSO_ZSTD, NRO_COMPRESSED = 1 << 7, 1 << 1
NAMES = ('text', 'ro', 'data')


class Error(ValueError):
    pass


def zstd_segment(raw, size, what):
    """A ZBIC segment (NSO flag bit 7, system 22.0.0 on) decompressed to size: zbic.decompress decodes Nintendo's zstd variant, and its own
    checks catch a damaged stream, so its messages are passed on; a wrong length still gets this loader's own Error, as a short LZ4 stream does"""
    try:
        import zbic   # pyright: ignore[reportMissingImports]  # an optional package, only a system 22.0.0+ module's ZBIC segments need it
    except ImportError:
        raise Error(f'{what}: ZBIC (zstd) segments need the zbic package (python3 -m pip install zbic)') from None
    try:
        out = zbic.decompress(bytes(raw))   # bytes: a caller may hand over the bytearray it built the module in
    except Exception as e:
        raise Error(f'{what}: ZBIC does not decompress ({e})') from None
    if len(out) != size: raise Error(f'{what}: ZBIC decompresses to {len(out):#x} bytes, not the {size:#x} of the header: the file is damaged')
    return out


class Module(NamedTuple):
    image: bytearray    # text + ro + data at their memory offsets
    segments: tuple     # ((memory offset, size),) of text, ro and data
    bss: int
    build_id: bytes     # 0x20 bytes; the build ID tools key on is its first 0x10

    @property
    def text_end(self): return self.segments[0][1]


def load(d, name='module'):
    """Module of the file's bytes d (NSO0 or NRO0); name goes into the Error messages"""
    if d[:4] == b'NSO0':
        if len(d) < 0x100: raise Error(f'{name}: the NSO header is cut short ({len(d)} bytes)')
        flags = struct.unpack_from('<I', d, 0xc)[0]
        stored = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]   # (file offset, memory offset, size)
        csz = [c if flags >> i & 1 else sz for i, ((_, _, sz), c) in enumerate(zip(stored, struct.unpack_from('<III', d, 0x60)))]   # bytes in the file
        bss, build_id = struct.unpack_from('<I', d, 0x3c)[0], bytes(d[0x40:0x60])
        for i, ((fo, mo, sz), c) in enumerate(zip(stored, csz)):
            what = f'{name}: {NAMES[i]} segment'
            if mo % PAGE: raise Error(f'{what} is at {mo:#x}, not on a page')
            if sz > SIZE_MAX: raise Error(f'{what} claims {sz:#x} bytes, more than Horizon loads')
            if fo + c > len(d): raise Error(f'{what} is stored at {fo:#x}..{fo + c:#x}, past the end of the file ({len(d):#x} bytes): the file is cut short or damaged')
            if flags >> i & 1 and c > sz: raise Error(f'{what} is stored compressed in {c:#x} bytes, more than its {sz:#x}')
            if flags >> i & 1 and not flags & NSO_ZSTD and sz > c * LZ4_MAX_RATIO: raise Error(f'{what} claims {sz:#x} bytes from {c:#x} stored, more than LZ4 can produce')
        if stored[0][1]: raise Error(f'{name}: text segment is at {stored[0][1]:#x}, not at 0')
        for i in (1, 2):
            if stored[i][1] < stored[i - 1][1] + stored[i - 1][2]: raise Error(f'{name}: {NAMES[i]} segment at {stored[i][1]:#x} overlaps the {NAMES[i - 1]} segment before it')
    elif d[0x10:0x14] == b'NRO0':
        # The file is the image itself: each segment stored at its memory offset, uncompressed; +0x18 the module's size, +0x1c flags, +0x20 text, +0x28 ro, +0x30 data as {offset, size},
        # +0x38 bss size, +0x40 the build ID
        if len(d) < 0x80: raise Error(f'{name}: the NRO header is cut short ({len(d)} bytes)')
        size, nflags = struct.unpack_from('<II', d, 0x18)
        if nflags & NRO_COMPRESSED: raise Error(f'{name}: a compressed NRO (system 23.0.0+), which this loader does not decode yet')
        flags, stored = 0, [(o, o, sz) for o, sz in (struct.unpack_from('<II', d, 0x20 + 8 * i) for i in range(3))]
        csz = [sz for _, _, sz in stored]
        bss, build_id = struct.unpack_from('<I', d, 0x38)[0], bytes(d[0x40:0x60])
        at = 0
        for i, (o, _, sz) in enumerate(stored):
            if sz % PAGE or o != at: raise Error(f'{name}: {NAMES[i]} segment {o:#x}..{o + sz:#x} is not whole pages right after the one before it (at {at:#x})')
            at += sz
        if at != size: raise Error(f'{name}: the segments end at {at:#x}, not at the module\'s size {size:#x}')
        if size > len(d): raise Error(f'{name}: the module is {size:#x} bytes, the file {len(d):#x}: the file is cut short or damaged')
    else:
        raise Error(f'{name}: not an NSO or NRO (magic {bytes(d[:4])!r})')
    end = stored[2][1] + stored[2][2]
    if end > len(d) * LZ4_MAX_RATIO: raise Error(f'{name}: the segments span {end:#x} bytes, more than a file of {len(d):#x} bytes can fill (a gap of zeros between them)')
    img = bytearray(end)
    for i, ((fo, mo, sz), c) in enumerate(zip(stored, csz)):
        raw = d[fo:fo + c]
        if flags >> i & 1:
            if flags & NSO_ZSTD:
                raw = zstd_segment(raw, sz, f'{name}: {NAMES[i]} segment')
            else:
                import lz4.block
                try: raw = lz4.block.decompress(raw, uncompressed_size=sz)
                except lz4.block.LZ4BlockError as e: raise Error(f'{name}: {NAMES[i]} segment does not decompress ({e})') from None
        if len(raw) != sz: raise Error(f'{name}: {NAMES[i]} segment decompresses to {len(raw):#x} bytes, not the {sz:#x} of the header: the file is damaged')
        if flags >> (3 + i) & 1 and hashlib.sha256(raw).digest() != d[0xa0 + 0x20 * i:0xc0 + 0x20 * i]:
            raise Error(f'{name}: {NAMES[i]} segment does not match its SHA-256 in the header: the file is damaged')
        img[mo:mo + sz] = raw
    return Module(img, tuple((mo, sz) for _, mo, sz in stored), bss, build_id)


def read(path):
    """load() of a file by path; Error carries the path"""
    with open(path, 'rb') as f: return load(f.read(), str(path))


def cli_read(path):
    """read() for a script's own command line: a damaged module stops it with a [RED] line, not a traceback"""
    try: return read(path)
    except Error as e: sys.exit(f'[RED] {e}')


if __name__ == '__main__':
    m = cli_read(sys.argv[1])
    with open(sys.argv[2], 'wb') as f:
        f.write(m.image); f.truncate(len(m.image) + m.bss)   # bss as a sparse zero tail, never allocated (Metroid Dread's main has about 1.8 GB)
    print([hex(x) for s in m.segments for x in s], hex(m.bss), 'moduleid', m.build_id.hex())
