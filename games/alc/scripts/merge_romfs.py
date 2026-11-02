#!/usr/bin/env python3
"""Apply the card update's BKTR patch over the verified base RomFS; verify the merged image's six IVFC levels; index it.

Astral Chain's card image holds the base Program NCA (a complete RomFS, already extracted and verified by
extract_romfs.py) and the update Program NCA (code plus a BKTR patch section). The patch section's indirect table
points into the base section or into the update's own AES-CTR-Ex data; the update section's IVFC header covers the
merged image, so a wrong layer fails a block hash. Keys stay in memory. No executable launch, no network.

Adapted from games/by3/scripts/merge_romfs.py (which merges a sparse base): the sparse layer is replaced by reads
from the already-verified base RomFS image, and both Program NCAs are selected with extract_exefs.program_nca.
"""
import argparse
import bisect
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_exefs import read_at, require, load_keys, container_entries, common_tickets, nca_inventory, program_nca, nca_ctr_key
from extract_romfs import index_romfs

NODE = 0x4000
CHUNK = 8 << 20


class BaseImage:
    """The verified, decrypted base RomFS (the base section's level-5 data). The indirect table's storage-0
    targets are base SECTION virtual offsets, and a section starts with its own hash tree, so reads subtract
    the base section's level-5 offset."""
    def __init__(self, path, l5_offset):
        self.path = path
        self.f = path.open('rb')
        self.size = path.stat().st_size
        self.l5_offset = l5_offset

    def read(self, offset, size):
        p = offset - self.l5_offset
        require(0 <= p and p + size <= self.size, 'Base RomFS read out of bounds')
        self.f.seek(p)
        data = self.f.read(size)
        require(len(data) == size, 'Truncated base RomFS read')
        return data


class Ctr:
    """AES-CTR over the NCA body at absolute NCA offsets; `upper` is the 8-byte big-endian upper IV."""
    def __init__(self, f, nca_offset, nca_size, key):
        self.f, self.base, self.size, self.key = f, nca_offset, nca_size, key

    def raw(self, absolute, size):
        require(0 <= absolute and absolute + size <= self.size, 'Read past the stored NCA')
        return read_at(self.f, self.base + absolute, size)

    def read(self, absolute, size, upper):
        aligned = absolute & ~15
        data = self.raw(aligned, size + absolute - aligned)
        from cryptography.hazmat.primitives.ciphers import modes
        from extract_exefs import decrypt
        return decrypt(self.key, modes.CTR(upper + (aligned // 16).to_bytes(8, 'big')), data)[absolute - aligned:]


def upper_iv(generation, secure):
    return struct.pack('>II', secure, generation)


def bucket_tree(read, size, header, entry_size, unpack):
    """Entries of a single-level bucket tree (L1 node + entry sets), each with its [start, end) virtual range."""
    magic, version, count = struct.unpack_from('<4sIi', header, 0)
    require(magic == b'BKTR' and version == 1 and count >= 0, 'Unsupported bucket tree header')
    if count == 0:
        return [], 0
    per_set = (NODE - 16) // entry_size
    sets = -(-count // per_set)
    require(sets <= (NODE - 16) // 8, 'Bucket trees with L2 nodes are unsupported')
    require(size == NODE * (1 + sets), 'Bucket tree size does not match its entry count')
    l1 = read(0, NODE)
    index, l1_count, total = struct.unpack_from('<iiq', l1, 0)
    require(index == 0 and l1_count == sets, 'Bucket tree L1 node')
    out = []
    for s in range(sets):
        node = read(NODE * (1 + s), NODE)
        index, n, end = struct.unpack_from('<iiq', node, 0)
        require(index == s and 0 < n <= per_set, 'Bucket tree entry set')
        first = struct.unpack_from('<q', l1, 16 + 8 * s)[0] if s else 0
        rows = [unpack(node, 16 + entry_size * k) for k in range(n)]
        require(rows[0][0] == first, 'Bucket tree entry set offset')
        for k, row in enumerate(rows):
            stop = rows[k + 1][0] if k + 1 < n else end
            require(row[0] < stop, 'Bucket tree entries out of order')
            out.append((row[0], stop) + tuple(row[1:]))
    require(len(out) == count and out[-1][1] == total, 'Bucket tree entry count / end offset')
    return out, total


def indirect_row(node, at):
    return struct.unpack_from('<qqi', node, at)


def ctr_ex_row(node, at):
    offset, encryption, generation = struct.unpack_from('<qB3xi', node, at)
    return offset, encryption, generation


class Ranges:
    def __init__(self, rows):
        self.rows, self.starts = rows, [r[0] for r in rows]

    def split(self, offset, size):
        """(row, offset within the row's range, length) pieces covering [offset, offset + size)."""
        i = bisect.bisect_right(self.starts, offset) - 1
        while size:
            require(0 <= i < len(self.rows) and self.rows[i][0] <= offset < self.rows[i][1], 'Read outside the table')
            row = self.rows[i]
            n = min(size, row[1] - offset)
            yield row, offset - row[0], n
            offset += n; size -= n; i += 1


class Patched:
    """The update's RomFS section: indirect table over (base image, AES-CTR-Ex update data)."""
    def __init__(self, ctr, start, end, fs, base):
        ind_off, ind_size = struct.unpack_from('<QQ', fs, 0x100)
        ex_off, ex_size = struct.unpack_from('<QQ', fs, 0x120)
        gen0, self.secure = struct.unpack_from('<II', fs, 0x140)
        require(fs[4] == 4, 'Expected an AES-CTR-Ex (BKTR) update section')
        require(ind_off + ind_size <= ex_off and ex_off + ex_size <= end - start, 'Patch tables out of bounds')
        # The AES-CTR-Ex table itself is plain AES-CTR; it covers everything before it, the indirect table included.
        plain = upper_iv(gen0, self.secure)
        ex_rows, ex_total = bucket_tree(lambda o, n: ctr.read(start + ex_off + o, n, plain), ex_size,
                                        fs[0x130:0x140], 0x10, ctr_ex_row)
        require(ex_total == ex_off, 'AES-CTR-Ex table must cover the section up to itself')
        self.ctr, self.start, self.base, self.ex = ctr, start, base, Ranges(ex_rows)
        ind_rows, total = bucket_tree(lambda o, n: self.update_data(ind_off + o, n), ind_size,
                                      fs[0x110:0x120], 0x14, indirect_row)
        require(all(r[3] in (0, 1) for r in ind_rows), 'Indirect storage index')
        self.total, self.ind, self.data_size = total, Ranges(ind_rows), ind_off
        self.from_base = self.from_update = 0

    def update_data(self, offset, size):
        out = bytearray()
        for (_, _, encryption, generation), _, n in self.ex.split(offset, size):
            absolute = self.start + offset + len(out)
            if encryption == 1:
                out += self.ctr.raw(absolute, n)
            else:
                require(encryption == 0, 'AES-CTR-Ex encryption value')
                out += self.ctr.read(absolute, n, upper_iv(generation, self.secure))
        return bytes(out)

    def read(self, offset, size):
        out = bytearray()
        for (_, _, target, storage), within, n in self.ind.split(offset, size):
            if storage == 0:
                out += self.base.read(target + within, n); self.from_base += n
            else:
                require(target + within + n <= self.data_size, 'Update data out of bounds')
                out += self.update_data(target + within, n); self.from_update += n
        return bytes(out)


def verify_ivfc(fs, size, read, image_path):
    """All six IVFC levels (as scripts/extract_romfs.py); level 6 is streamed to image_path."""
    require(fs[8:12] == b'IVFC' and struct.unpack_from('<III', fs, 12) == (0x20000, 32, 7), 'Unsupported IVFC layout')
    levels = [struct.unpack_from('<QQII', fs, 0x18 + j * 24) for j in range(6)]
    parent, counts, digest = fs[0xc8:0xe8], [], hashlib.sha256()
    with image_path.open('wb') as image:
        for level, (offset, length, power, reserved) in enumerate(levels):
            require(12 <= power <= 24 and reserved == 0 and offset + length <= size, 'IVFC level layout')
            block = 1 << power
            require(len(parent) >= (length + block - 1) // block * 32, 'IVFC parent size')
            captured = bytearray() if level < 5 else None
            index = 0
            for pos in range(0, length, CHUNK):
                chunk = read(offset + pos, min(CHUNK, length - pos))
                for j in range(0, len(chunk), block):
                    payload = chunk[j:j + block]
                    require(hashlib.sha256(payload + bytes(block - len(payload))).digest() == parent[index * 32:index * 32 + 32],
                            f'IVFC level {level} block {index} failed')
                    index += 1
                if captured is not None:
                    captured.extend(chunk)
                else:
                    image.write(chunk); digest.update(chunk)
                    if pos % (1 << 30) < CHUNK:
                        print(f'IVFC level {level}: {pos >> 30} / {length >> 30} GiB', flush=True)
            counts.append(index)
            if captured is not None:
                parent = bytes(captured)
            print(f'IVFC level {level}: {index} blocks verified', flush=True)
    return counts, levels[-1][1], digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='the game file (.xci or .nsp) holding base and update Program NCAs')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title', required=True, help='base title ID')
    parser.add_argument('--update-title', required=True, help='update title ID')
    parser.add_argument('--base-romfs', required=True, type=Path,
                        help='the verified base RomFS image from extract_romfs.py (its SHA-256 is recorded in the report)')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists; choose a new directory')
    keys = load_keys(args.keys)
    base_sha = hashlib.sha256(args.base_romfs.read_bytes()).hexdigest() if args.base_romfs.stat().st_size < (1 << 31) else None
    if base_sha is None:
        h = hashlib.sha256()
        with args.base_romfs.open('rb') as f:
            for pos in range(0, args.base_romfs.stat().st_size, CHUNK):
                h.update(f.read(min(CHUNK, args.base_romfs.stat().st_size - pos)))
        base_sha = h.hexdigest()
    with args.game.open('rb') as f:
        _, entries = container_entries(f, args.game.stat().st_size)
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        base_rec, base_hdr = program_nca(records, title=args.title, update_title=args.update_title)
        upd_rec, upd_hdr = program_nca(records, update=True, title=args.title, update_title=args.update_title)
        uctr = Ctr(f, upd_rec['nca_offset'], upd_rec['size'], nca_ctr_key(upd_hdr, keys, tickets))
        # The update's RomFS (BKTR) section: hash type 3 with CTR-Ex crypto.
        ustart = uend = ufs = None
        for i in range(4):
            start, end = struct.unpack_from('<II', upd_hdr, 0x240 + 16 * i)
            fs = upd_hdr[0x400 + 512 * i:0x600 + 512 * i]
            if start and fs[3] == 3:
                require(hashlib.sha256(fs).digest() == upd_hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
                ustart, uend, ufs = start * 512, end * 512, fs
                break
        require(ufs is not None, 'No RomFS (BKTR) section in the update NCA')
        # The base section's own level-5 offset: where its RomFS data begins inside the base section.
        bl5_offset = None
        for i in range(4):
            start, _ = struct.unpack_from('<II', base_hdr, 0x240 + 16 * i)
            bfs = base_hdr[0x400 + 512 * i:0x600 + 512 * i]
            if start and bfs[3] == 3:
                bl5_offset = struct.unpack_from('<Q', bfs, 0x18 + 5 * 24)[0]
                break
        require(bl5_offset is not None, 'No RomFS section in the base NCA')
        patched = Patched(uctr, ustart, uend, ufs, BaseImage(args.base_romfs, bl5_offset))
        args.output.mkdir(parents=True)
        image = args.output / 'romfs.bin'
        counts, size, sha = verify_ivfc(ufs, patched.total, patched.read, image)
        files = index_romfs(image)
        report = dict(status='pass',
                      game=dict(source=str(args.game.resolve()), source_size=args.game.stat().st_size),
                      base_program=base_rec, update_program=upd_rec,
                      base_romfs=dict(path=str(args.base_romfs.resolve()), size=args.base_romfs.stat().st_size, sha256=base_sha),
                      ivfc_blocks=counts, romfs_size=size, romfs_sha256=sha,
                      bytes_from_base=patched.from_base, bytes_from_update=patched.from_update,
                      files=files,
                      integrity='FS headers and all six IVFC levels of the merged image verified; RSA signatures not checked')
        (args.output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(status='pass', file_count=len(files), size=size, sha256=sha,
                              bytes_from_base=patched.from_base, bytes_from_update=patched.from_update)), flush=True)


if __name__ == '__main__':
    main()
