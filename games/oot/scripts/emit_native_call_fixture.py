#!/usr/bin/env python3
"""Synthetic caller using the same BLR-x18 preparation emitted for TOTK."""
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
from call_window import prepare_call, branch, BLR_X30

blob = bytearray(0xC000)
caller = [0xA9BC7BFD, 0x910003FD, 0xAA0203F1, 0xAA0303EF,
          branch(0x110, 0x2000), BLR_X30, 0xA8C47BFD, 0xD65F03C0]
blob[0x100:0x120] = struct.pack('<8I', *caller)
block = prepare_call(0xA901BFF1, 0x110, 0x114, 0x2000, 0x4000, 0)
blob[0x2000:0x2000 + len(block)] = block
Path(sys.argv[1]).write_bytes(blob)
