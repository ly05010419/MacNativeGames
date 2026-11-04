#!/usr/bin/env python3
"""Check SURED lowering using a private captured job.

Usage: check_sured_service.py job.json   (after make shader-tools). No GPU work is submitted.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from shader_tools import service_command

job = json.loads(Path(sys.argv[1]).read_text())[0]
original = Path(job['code']).read_bytes()
with tempfile.TemporaryDirectory(prefix='mkw-sured-') as directory:
    out = Path(directory)
    job['variant'] = job['key']
    def request(value):
        result = subprocess.run(service_command(out, 'mkw-sm86-v5-unverified'), input=json.dumps(value) + '\n', text=True, capture_output=True, env=os.environ.copy(), check=True)
        return json.loads(result.stdout)
    reply = request(job)
    assert reply['ok'], reply
    lowered = (out / (job['key'] + '.sured.code')).read_bytes()
    assert len(original) == len(lowered)
    changed = []
    for offset in range(0, len(original), 16):
        a, b = original[offset:offset+16], lowered[offset:offset+16]
        if a == b:
            continue
        lo, hi = struct.unpack('<QQ', a)
        nlo, nhi = struct.unpack('<QQ', b)
        assert lo & 4095 == 0x59f
        assert nlo == (lo & ~0xff0fff) | 0xff0000 | 0x593
        assert nhi == (hi & ~(7 << 17)) | (7 << 17)
        changed.append(offset)
    assert changed
    source = (out / (job['key'] + '_b.metal')).read_text()
    assert '.atomic_fetch_add(' in source and '_atomic [[' not in source
    bad = bytearray(original)
    # Change the surface dimension to 2D array: this form has not been implemented.
    lo = struct.unpack_from('<Q', bad, changed[0])[0]
    struct.pack_into('<Q', bad, changed[0], (lo & ~(7 << 61)) | (4 << 61))
    bad_code = out / 'unsupported.code'; bad_code.write_bytes(bad)
    job['code'] = str(bad_code); job['variant'] = job['key'] + '-unsupported'
    reply = request(job)
    assert not reply['ok'], reply
    print('SURED: targeted lowering, byte preservation, native texture atomic and unsupported-mode rejection passed')
