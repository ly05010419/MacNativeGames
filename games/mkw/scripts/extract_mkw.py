#!/usr/bin/env python3
"""Mario Kart World: the ExeFS and the RomFS index from an NSP of your own dump whose NCAs you decrypted (NCA3 headers
and sections decrypted, no key area), read in place: nothing is decrypted here and the 23 GB RomFS is not copied.

    extract_mkw.py GAME.nsp --title-id 0400c3f00006e000 --exefs extracted/mkw/exefs --romfs extracted/mkw/romfs

The program NCA's header holds the SHA-256 of each FS header; the ExeFS PFS0 is checked against its hierarchical SHA-256
table, and the RomFS against all six IVFC levels. The RomFS folder gets index.json (as extract_romfs.py writes it) and
romfs.source ("<offset> <path>": where the image lies in the NSP, runtime/romfs.c), not romfs.bin. The NCA header's RSA
signature is not checked."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def require(ok, message):
    if not ok:
        raise SystemExit(f'error: {message}')


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    require(len(data) == size, f'short read at {offset:#x}')
    return data


def nsp_entries(f):
    head = read_at(f, 0, 16)
    require(head[:4] == b'PFS0', 'not an NSP (PFS0)')
    count, strings = struct.unpack_from('<II', head, 4)
    table = read_at(f, 16, 24 * count)
    names = read_at(f, 16 + 24 * count, strings)
    base = 16 + 24 * count + strings
    out = []
    for i in range(count):
        offset, size, name = struct.unpack_from('<QQI', table, 24 * i)
        out.append((names[name:names.index(b'\0', name)].decode(), base + offset, size))
    return out


def program_nca(f, title_id):
    """The one plain Program NCA (content type 0) of the title."""
    found = []
    for name, offset, size in nsp_entries(f):
        if not name.endswith('.nca') or name.endswith('.cnmt.nca') or size < 0xc00:
            continue
        hdr = read_at(f, offset, 0xc00)
        require(hdr[0x200:0x204] == b'NCA3', f'{name}: the NCA header is encrypted; this extractor reads only decrypted NCAs')
        if hdr[0x205] == 0 and struct.unpack_from('<Q', hdr, 0x210)[0] == title_id:
            require(hdr[0x230:0x240] == bytes(16) and hdr[0x300:0x340] == bytes(64), f'{name}: titlekey or key area set')
            found.append((name, offset, size, hdr))
    require(len(found) == 1, f'expected one Program NCA of {title_id:016x}, found {len(found)}')
    return found[0]


def sections(hdr):
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
        if not end:
            continue
        fs = hdr[0x400 + 0x200 * i:0x600 + 0x200 * i]
        require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], f'section {i}: FS header hash')
        require(fs[4] == 1, f'section {i}: encrypted (type {fs[4]})')
        yield i, start * 0x200, end * 0x200, fs


def extract_exefs(f, nca, hdr, out):
    for i, start, end, fs in sections(hdr):
        if fs[2] != 1 or fs[3] != 2:   # PartitionFs with a hierarchical SHA-256 table
            continue
        master = fs[0x8:0x28]
        block = struct.unpack_from('<I', fs, 0x28)[0]
        require(struct.unpack_from('<I', fs, 0x2c)[0] == 2, 'ExeFS hash layer count')
        (toff, tsize), (poff, psize) = struct.unpack_from('<QQ', fs, 0x30), struct.unpack_from('<QQ', fs, 0x40)
        table = read_at(f, nca + start + toff, tsize)
        require(hashlib.sha256(table).digest() == master, 'ExeFS hash table')
        pfs = read_at(f, nca + start + poff, psize)
        for k in range(0, psize, block):
            require(hashlib.sha256(pfs[k:k + block]).digest() == table[k // block * 32:k // block * 32 + 32], f'ExeFS block {k // block}')
        require(pfs[:4] == b'PFS0', 'ExeFS PFS0')
        count, strings = struct.unpack_from('<II', pfs, 4)
        names = pfs[16 + 24 * count:16 + 24 * count + strings]
        data = 16 + 24 * count + strings
        out.mkdir(parents=True, exist_ok=True)
        files = {}
        for n in range(count):
            offset, size, name = struct.unpack_from('<QQI', pfs, 16 + 24 * n)
            name = names[name:names.index(b'\0', name)].decode()
            if not (name in ('main', 'main.npdm', 'rtld', 'sdk') or name.startswith('subsdk')):
                print(f'ExeFS: {name} skipped (not a module; a repacking tool may add a listing)', flush=True)
                continue
            body = pfs[data + offset:data + offset + size]
            (out / name).write_bytes(body)
            files[name] = hashlib.sha256(body).hexdigest()
        print(json.dumps(dict(exefs=files)), flush=True)
        return files
    require(False, 'no ExeFS section')


def verify_romfs(f, nca, hdr):
    """All six IVFC levels of the RomFS section; returns (absolute offset of the image in the file, its size, its SHA-256)."""
    for i, start, end, fs in sections(hdr):
        if fs[2] != 0 or fs[3] != 3:
            continue
        require(fs[8:12] == b'IVFC' and struct.unpack_from('<III', fs, 12) == (0x20000, 32, 7), 'unsupported IVFC layout')
        levels = [struct.unpack_from('<QQII', fs, 0x18 + 24 * j) for j in range(6)]
        parent = fs[0xc8:0xe8]
        image = digest = None
        for level, (offset, size, power, reserved) in enumerate(levels):
            require(12 <= power <= 24 and reserved == 0 and offset + size <= end - start, 'IVFC level bounds')
            block = 1 << power
            require(len(parent) >= (size + block - 1) // block * 32, 'IVFC parent size')
            captured = bytearray() if level < 5 else None
            digest = hashlib.sha256() if level == 5 else None
            index = 0
            f.seek(nca + start + offset)
            for pos in range(0, size, 32 << 20):
                chunk = f.read(min(32 << 20, size - pos))
                require(len(chunk) == min(32 << 20, size - pos), 'short RomFS read')
                for j in range(0, len(chunk), block):
                    payload = chunk[j:j + block]
                    if len(payload) < block:
                        payload += bytes(block - len(payload))
                    require(hashlib.sha256(payload).digest() == parent[index * 32:index * 32 + 32], f'IVFC level {level} block {index} failed')
                    index += 1
                if captured is not None:
                    captured.extend(chunk)
                else:
                    digest.update(chunk)
                    if pos % (1 << 30) < (32 << 20):
                        print(f'IVFC level 5: {pos / size * 100:.0f}%', flush=True)
            if captured is not None:
                parent = bytes(captured)
            print(f'IVFC level {level}: {index} blocks verified', flush=True)
            image = (nca + start + offset, size)
        return image[0], image[1], digest.hexdigest()
    require(False, 'no RomFS section')


class Window:
    """A file seen from an offset: what index_romfs reads as the image."""
    def __init__(self, f, base, size):
        self.f, self.base, self.size = f, base, size

    def read_at(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'RomFS read outside the image')
        return read_at(self.f, self.base + offset, size)


def index_romfs(img):
    """extract_romfs.py index_romfs, over the image in place."""
    h = struct.unpack('<10Q', img.read_at(0, 80))
    require(h[0] == 80, 'RomFS header')
    length = img.size
    for off, size in [(h[1], h[2]), (h[3], h[4]), (h[5], h[6]), (h[7], h[8])]:
        require(off + size <= length, 'RomFS table bounds')
    dirs = img.read_at(h[3], h[4])
    entries = img.read_at(h[7], h[8])
    files, seen_dirs, seen_files, paths = [], set(), set(), set()

    def name(table, off, n):
        require(off + n <= len(table), 'Name bounds')
        s = table[off:off + n].decode('utf-8')
        require(s not in ['.', '..'] and '/' not in s and '\\' not in s and '\0' not in s, 'Unsafe path')
        return s
    todo = [(0, '')]
    while todo:
        off, parent = todo.pop()
        require(off not in seen_dirs and off + 24 <= len(dirs), 'Invalid/cyclic directory')
        seen_dirs.add(off)
        _, sibling, child, file, _, n = struct.unpack_from('<6I', dirs, off)
        part = name(dirs, off + 24, n)
        prefix = parent + part + '/' if part else parent
        if sibling != 0xffffffff:
            todo.append((sibling, parent))
        if child != 0xffffffff:
            todo.append((child, prefix))
        while file != 0xffffffff:
            require(file not in seen_files and file + 32 <= len(entries), 'Invalid/cyclic file')
            seen_files.add(file)
            _, next_file, data, size, _, n = struct.unpack_from('<IIQQII', entries, file)
            part = name(entries, file + 32, n)
            require(part != '', 'Empty file name')
            filename = prefix + part
            require(filename not in paths and h[9] + data + size <= length, 'Duplicate/out-of-bounds file')
            paths.add(filename)
            files.append(dict(path=filename, offset=h[9] + data, size=size))
            file = next_file
    return sorted(files, key=lambda item: item['path'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game', type=Path)
    ap.add_argument('--title-id', required=True, type=lambda s: int(s, 16))
    ap.add_argument('--exefs', type=Path)
    ap.add_argument('--romfs', type=Path)
    a = ap.parse_args()
    game = a.game.resolve()
    with game.open('rb') as f:
        name, nca, size, hdr = program_nca(f, a.title_id)
        record = dict(source=str(game), source_size=game.stat().st_size, program=name, title_id=f'{a.title_id:016x}')
        if a.exefs:
            record['exefs_sha256'] = extract_exefs(f, nca, hdr, a.exefs)
            (a.exefs.parent / 'extraction-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
        if a.romfs:
            offset, length, sha = verify_romfs(f, nca, hdr)
            files = index_romfs(Window(f, offset, length))
            a.romfs.mkdir(parents=True, exist_ok=True)
            (a.romfs / 'romfs.source').write_text(f'{offset} {game}\n')
            (a.romfs / 'index.json').write_text(json.dumps(dict(status='pass', **record, romfs_offset=offset, romfs_size=length,
                                                                romfs_sha256=sha, files=files,
                                                                integrity='FS header and all six IVFC levels verified; RSA signature not checked'), indent=2) + '\n')
            print(json.dumps(dict(status='pass', file_count=len(files), size=length, sha256=sha, offset=offset)), flush=True)


if __name__ == '__main__':
    main()
