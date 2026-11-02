#!/usr/bin/env python3
"""Merge the base Program NCA and its BKTR update into one RomFS image; verify all six IVFC levels; index it.

Super Mario Bros. Wonder's base NSP holds a complete Program NCA (copied from games/ssbu, which keeps games/by3's sparse case). The
layers here are, from the bottom up:
  base:   NCA body -> AES-CTR (the section's own upper IV); a sparse base adds an indirect table (data / zero) below it
  update: NCA body -> AES-CTR-Ex (per-entry generation) ; indirect table -> base section or update data
The IVFC header of the update section covers the merged image, so a wrong layer fails a block hash.
Keys stay in memory. No executable launch, no network.
"""
import argparse
import bisect
import hashlib
import json
import struct
import sys
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import modes

sys.path.insert(0, str(Path(__file__).resolve().parent))   # extract_exefs / extract_romfs: this folder's copies of games/totk/scripts
from extract_exefs import decrypt, read_at, require, load_keys, container_entries, common_tickets, nca_ctr_key
from extract_romfs import index_romfs

BASE_TITLE = '0100B99019412000'
NODE = 0x4000
CHUNK = 8 << 20


def program_header(f, entries, keys, title):
    """The single Program NCA of `title`. Sparse sections may end past the stored NCA, so bounds are checked per layer."""
    found = []
    for name, offset, length in entries:
        if not name.lower().endswith('.nca') or name.lower().endswith('.cnmt.nca'):
            continue
        raw = read_at(f, offset, 0xc00)
        hdr = b''.join(decrypt(keys['header_key'], modes.XTS(i.to_bytes(16, 'big')), raw[i * 512:(i + 1) * 512])
                       for i in range(6))
        require(hdr[0x200:0x204] == b'NCA3', 'Expected NCA3 (check prod.keys and the game file)')
        require(struct.unpack_from('<Q', hdr, 0x208)[0] == length, 'NCA size does not match container entry')
        if hdr[0x205] == 0 and f'{struct.unpack_from("<Q", hdr, 0x210)[0]:016X}' == title:
            found.append((dict(name=name, nca_offset=offset, size=length), hdr))
    require(len(found) == 1, f'Expected one Program NCA for {title}')
    return found[0]


def romfs_section(hdr):
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
        fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
        if start and fs[2:4] == b'\x00\x03':
            require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
            return start * 512, end * 512, fs
    raise ValueError('No RomFS section')


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


class PlainBase:
    """A complete base RomFS section: plain AES-CTR with the section's own upper IV."""
    def __init__(self, ctr, start, end, fs):
        require(fs[4] == 3, 'Expected an AES-CTR base RomFS section')
        gen0, secure = struct.unpack_from('<II', fs, 0x140)
        self.ctr, self.start, self.size, self.upper, self.stored = ctr, start, end - start, upper_iv(gen0, secure), 0

    def read(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'Base RomFS read out of bounds')
        self.stored += size
        return self.ctr.read(self.start + offset, size, self.upper)


def base_section(ctr, start, end, fs):
    """The sparse reader when the section carries a sparse table (generation at 0x170), the plain one otherwise."""
    return SparseBase(ctr, start, end, fs) if struct.unpack_from('<QH', fs, 0x168)[1] else PlainBase(ctr, start, end, fs)


class SparseBase:
    """The base RomFS section as it was before sparsing; parts the update replaces read as zeros."""
    def __init__(self, ctr, start, end, fs):
        boff, bsize = struct.unpack_from('<QQ', fs, 0x148)
        self.phys, generation = struct.unpack_from('<QH', fs, 0x168)
        require(generation != 0, 'Base RomFS is not sparse; use the standard extractor')
        # The table's counter uses the section's offset; verified on Bayonetta 3, where the two offsets are equal.
        require(self.phys == start, 'Sparse layout with a moved section start is untested')
        gen0, self.secure = struct.unpack_from('<II', fs, 0x140)
        self.ctr, self.start, self.upper = ctr, start, upper_iv(gen0, self.secure)
        meta_upper = upper_iv(generation << 16, self.secure)
        rows, total = bucket_tree(lambda o, n: ctr.read(self.phys + boff + o, n, meta_upper), bsize,
                                  fs[0x158:0x168], 0x14, indirect_row)
        require(total == end - start, 'Sparse table does not cover the section')
        require(all(r[3] in (0, 1) for r in rows), 'Sparse storage index')
        self.map, self.data_size, self.stored = Ranges(rows), boff, 0

    def read(self, offset, size):
        out = bytearray()
        for (_, _, phys, storage), within, n in self.map.split(offset, size):
            if storage == 1:
                out += bytes(n); continue
            p = phys + within
            require(p + n <= self.data_size, 'Sparse data out of bounds')
            # Stored blocks are the original ciphertext, so they decrypt at their original (virtual) offset. CTR works
            # byte by byte: padding to the block boundary with any bytes and dropping them gives the same plaintext.
            absolute = self.start + offset + len(out)
            pad = absolute & 15
            data = bytes(pad) + self.ctr.raw(self.phys + p, n)
            out += decrypt(self.ctr.key, modes.CTR(self.upper + ((absolute - pad) // 16).to_bytes(8, 'big')), data)[pad:]
            self.stored += n
        return bytes(out)


class Patched:
    """The update's RomFS section: indirect table over (base section, AES-CTR-Ex update data)."""
    def __init__(self, ctr, start, end, fs, base):
        ind_off, ind_size = struct.unpack_from('<QQ', fs, 0x100)
        ex_off, ex_size = struct.unpack_from('<QQ', fs, 0x120)
        gen0, self.secure = struct.unpack_from('<II', fs, 0x140)
        require(fs[4] == 4, 'Expected an AES-CTR-Ex (BKTR) update section')
        require(ind_off + ind_size <= ex_off and ex_off + ex_size <= end - start, 'Patch tables out of bounds')
        # The AES-CTR-Ex table itself is plain AES-CTR. Its extent says what it covers: up to itself, the indirect table
        # included (Bayonetta 3 1.2.0), or only the data before the indirect table, which is then plain AES-CTR too
        # (Super Mario Odyssey 1.4.1). A wrong choice fails the indirect table's bucket checks or the IVFC hashes.
        plain = upper_iv(gen0, self.secure)
        ex_rows, ex_total = bucket_tree(lambda o, n: ctr.read(start + ex_off + o, n, plain), ex_size,
                                        fs[0x130:0x140], 0x10, ctr_ex_row)
        require(ex_total in (ex_off, ind_off), 'AES-CTR-Ex table must cover the section up to itself or up to the indirect table')
        self.ctr, self.start, self.base, self.ex = ctr, start, base, Ranges(ex_rows)
        read_table = (lambda o, n: self.update_data(ind_off + o, n)) if ex_total == ex_off else \
                     (lambda o, n: ctr.read(start + ind_off + o, n, plain))
        ind_rows, total = bucket_tree(read_table, ind_size, fs[0x110:0x120], 0x14, indirect_row)
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
    """All six IVFC levels (as games/totk/scripts/extract_romfs.py); level 6 is streamed to image_path."""
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


def open_program(path, keys, title):
    f = path.open('rb')
    _, entries = container_entries(f, path.stat().st_size)
    record, hdr = program_header(f, entries, keys, title)
    key = nca_ctr_key(hdr, keys, common_tickets(f, entries))
    return f, record, hdr, Ctr(f, record['nca_offset'], record['size'], key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path, help='base game NSP')
    parser.add_argument('update', type=Path, help='update NSP (BKTR Program NCA)')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title', default=BASE_TITLE, help='program title ID (default: Mario vs. Donkey Kong)')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists; choose a new directory')
    keys = load_keys(args.keys)
    bf, brec, bhdr, bctr = open_program(args.base, keys, args.title)
    uf, urec, uhdr, uctr = open_program(args.update, keys, args.title)
    with bf, uf:
        base = base_section(bctr, *romfs_section(bhdr))
        ustart, uend, ufs = romfs_section(uhdr)
        patched = Patched(uctr, ustart, uend, ufs, base)
        args.output.mkdir(parents=True)
        image = args.output / 'romfs.bin'
        counts, size, sha = verify_ivfc(ufs, patched.total, patched.read, image)
        files = index_romfs(image)
        report = dict(status='pass', base=dict(source=str(args.base.resolve()), source_size=args.base.stat().st_size, program=brec),
                      update=dict(source=str(args.update.resolve()), source_size=args.update.stat().st_size, program=urec),
                      ivfc_blocks=counts, romfs_size=size, romfs_sha256=sha,
                      bytes_from_base=patched.from_base, bytes_from_update=patched.from_update, base_stored_bytes_read=base.stored,
                      files=files, integrity='FS headers and all six IVFC levels of the merged image verified; RSA signatures not checked')
        (args.output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(status='pass', file_count=len(files), size=size, sha256=sha,
                              bytes_from_base=patched.from_base, bytes_from_update=patched.from_update)), flush=True)


if __name__ == '__main__':
    main()
