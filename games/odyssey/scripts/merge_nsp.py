#!/usr/bin/env python3
"""Join a base-game NSP and a separate update NSP into one NSP (PFS0), the single game file the build reads.

    python3 scripts/merge_nsp.py BASE.nsp UPDATE.nsp --output Odyssey_build/input/odyssey.nsp

Every entry of both files is copied unchanged (NCAs, tickets, certificates); a name both files carry must hold the same
bytes. Nothing is decrypted, and no keys are needed. The output is written next to itself and renamed when complete.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "third_party" / "totk-scripts"))
from extract_exefs import partition_entries, require  # noqa: E402

CHUNK = 8 << 20


def digest(f, start, length):
    h = hashlib.sha256()
    f.seek(start)
    while length:
        block = f.read(min(CHUNK, length))
        require(block, "Truncated input")
        h.update(block)
        length -= len(block)
    return h.digest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("nsp", type=Path, nargs="+", help="the base NSP, then the update NSP")
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    require(len(args.nsp) >= 2, "Give the base NSP and the update NSP")

    sources = [path.open("rb") for path in args.nsp]
    entries, seen = [], {}
    for index, (path, f) in enumerate(zip(args.nsp, sources)):
        size = path.stat().st_size
        require(f.read(4) == b"PFS0", f"{path.name}: not an NSP (PFS0)")
        for name, start, length in partition_entries(f, 0, size):
            if name in seen:
                other = entries[seen[name]]
                require(other[3] == length and digest(sources[other[0]], other[2], length) == digest(f, start, length),
                        f"{name} differs between {args.nsp[other[0]].name} and {path.name}")
                continue
            seen[name] = len(entries)
            entries.append((index, name, start, length))

    strings = b"".join(name.encode() + b"\0" for _, name, _, _ in entries)
    strings += b"\0" * (-(16 + 24 * len(entries) + len(strings)) % 0x20)
    table, offset, name_at = [], 0, 0
    for _, name, _, length in entries:
        table.append(struct.pack("<QQII", offset, length, name_at, 0))
        offset += length
        name_at += len(name.encode()) + 1
    header = b"PFS0" + struct.pack("<II", len(entries), len(strings)) + b"\0" * 4 + b"".join(table) + strings

    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_name(args.output.name + ".partial")
    with partial.open("wb") as out:
        out.write(header)
        for index, _, start, length in entries:
            f = sources[index]
            f.seek(start)
            while length:
                block = f.read(min(CHUNK, length))
                require(block, "Truncated input")
                out.write(block)
                length -= len(block)
    for f in sources:
        f.close()
    with partial.open("rb") as f:
        require(len(partition_entries(f, 0, partial.stat().st_size)) == len(entries), "Merged NSP does not read back")
    partial.replace(args.output)
    print(f"{len(entries)} entries from {len(args.nsp)} NSPs -> {args.output} ({args.output.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
