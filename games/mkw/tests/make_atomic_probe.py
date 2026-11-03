#!/usr/bin/env python3
"""Generate a synthetic SM86 image-atomic job. No game shader code is used.

Each invocation adds 1 to image 0 at coordinate (0,0); dispatch 64 threads to expect +64.
"""
import json
import struct
import sys
from pathlib import Path

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
instructions = []
for register, value in ((0, 1), (4, 0), (5, 0)):
    # IMAD RZ * RZ + immediate, PT execution predicate.
    instructions.append((0x424 | 0x7000 | register << 16 | 255 << 24 | value << 32, 0x078e00ff))
if len(sys.argv) > 2:
    # LDC.U32 R0, c0[header offset]. Used to exercise driver-header translation.
    instructions[0] = (0xb82 | 0x7000 | 255 << 24 | int(sys.argv[2]) << 38, 4 << 9)
# SURED.D.2D.ADD.U32.STRONG.GPU; descriptor c0[0x10] maps to image 0.
instructions.append((0x59f | 0x7000 | 4 << 24 | 4 << 40 | 3 << 61, 0x0010e000))
instructions.append((0x94d | 0x7000, 7 << 23))
code = out / 'atomic-probe.code'
# No dependency barriers: otherwise the frontend defers register writes until a
# matching wait, and the atomic would read the initial register values.
code.write_bytes(b''.join(struct.pack('<QQ', lo, hi | (7 << 46) | (7 << 49)) for lo, hi in instructions))
job = {'key': 'atomic-probe', 'variant': 'atomic-probe', 'arch': 'sm86', 'stage': 'cs', 'code': str(code), 'nvn2_cs': [0, 0, 0, 24, 0], 'local': [64, 1, 1], 'tex': {}, 'img': {'0': {'type': '2d', 'fmt': 'R32Uint'}}, 'attr': {}, 'fout': {}}
(out / 'request.json').write_text(json.dumps(job) + '\n')
print(out / 'request.json')
