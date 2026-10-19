#!/usr/bin/env python3
"""Inspect or change TOTK 1.0.0 / 1.1.2 / 1.2.1 player position in a *copy* of progress.sav.

1.2.1 writes the 1.1.2 layout (2,307,656 bytes, header 0x0047E0F4; checked 2026-10-19).

Both World_PlayerPos and PlayerStatus.SavePos are raw X,Y,Z vectors; Y is
height. Both must be updated so world streaming follows the new position.
Never write into the source profile.
"""
import argparse
import json
import math
import struct
from pathlib import Path

POSITION_HASH = 0xC884818D
WORLD_POSITION_HASH = 0x8773F02D
END_HASH = 0xA3DB7114
EXPECTED_SIZE = 2307552
EXPECTED_HEADER = 0x0046C3C8
SAVE_LAYOUTS = {(EXPECTED_SIZE, EXPECTED_HEADER), (2307656, 0x0047E0F4)}


def locate_all(data):
    if len(data) < 8 or struct.unpack_from("<I", data)[0] != 0x01020304 or (len(data), struct.unpack_from("<I", data, 4)[0]) not in SAVE_LAYOUTS:
        raise ValueError("requires a verified TOTK 1.0.0 or 1.1.2 / 1.2.1 progress.sav layout")
    offsets = {}
    for index in range(0x28, len(data) - 8, 8):
        tag, value = struct.unpack_from("<II", data, index)
        if tag in (POSITION_HASH, WORLD_POSITION_HASH):
            if value < 0x10000 or value + 12 > len(data) or value & 3:
                raise ValueError("invalid player position pointer")
            offsets[tag] = value
        if tag == END_HASH:
            break
    if set(offsets) != {POSITION_HASH, WORLD_POSITION_HASH}:
        raise ValueError("both PlayerStatus.SavePos and World_PlayerPos are required")
    return offsets[POSITION_HASH], offsets[WORLD_POSITION_HASH]


def locate(data):
    return locate_all(data)[0]


def position(data, offset):
    return list(struct.unpack_from("<fff", data, offset))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, help="required for a changed copy; must differ from source")
    parser.add_argument("--xyz", nargs=3, type=float, metavar=("X", "Y", "Z"))
    args = parser.parse_args()
    data = bytearray(args.source.read_bytes())
    offset, world_offset = locate_all(data)
    if data[offset:offset+12] != data[world_offset:world_offset+12]:
        raise ValueError("save and world positions differ; refusing an ambiguous edit")
    before = position(data, offset)
    if args.xyz is None:
        print(json.dumps({"source": str(args.source), "offset": offset, "xyz": before}))
        return
    if args.output is None or args.output.resolve() == args.source.resolve():
        parser.error("changing coordinates requires --output at a different path")
    x, y, z = args.xyz
    if not all(math.isfinite(v) for v in (x, y, z)) or not (-5500 <= x <= 5500 and -1500 <= y <= 3500 and -5000 <= z <= 5000):
        parser.error("coordinates outside a plausible TOTK world range")
    for at in (offset, world_offset):
        struct.pack_into("<fff", data, at, x, y, z)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    after = position(data, offset)
    print(json.dumps({"source": str(args.source), "output": str(args.output), "offset": offset, "before": before, "after": after}))


if __name__ == "__main__":
    main()
