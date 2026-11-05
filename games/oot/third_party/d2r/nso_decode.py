"""Decode original NSO segments. ZBIC is an external build utility, never part of the native runtime."""
import os
import subprocess
import sys
import lz4.block


def decompress_segment(payload, flags, expected):
    if flags & 0x80:
        # SDK 22's NSO bit 7 selects Nintendo's ZBIC encoding, not stock zstd/LZ4.
        # Run the separately installed GPLv2 utility as a build tool; no implementation is copied here.
        python = os.environ.get('OOT_ZBIC_PYTHON', sys.executable)
        result = subprocess.run([python, '-c',
            'import sys,zbic;sys.stdout.buffer.write(zbic.decompress(sys.stdin.buffer.read()))'],
            input=payload, capture_output=True, check=False)
        if result.returncode:
            raise ValueError('ZBIC build utility failed: ' + result.stderr.decode(errors='replace')[-1000:])
        data = result.stdout
    else:
        data = lz4.block.decompress(payload, uncompressed_size=expected)
    if len(data) != expected:
        raise ValueError(f'NSO segment expands to {len(data)}, expected {expected}')
    return data
