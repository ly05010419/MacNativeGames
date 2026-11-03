#!/usr/bin/env python3
"""Translate and GPU-check a synthetic SM86 atomic, without private game data.

Usage: run_atomic_probe.py service spirv-cross
Set DOTNET_ROOT and MKW_DMSH_XLATE as for the service. Requires Metal access.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='mkw-atomic-') as directory:
    out = Path(directory)
    subprocess.run([sys.executable, str(here / 'make_atomic_probe.py'), str(out)], check=True)
    result = subprocess.run(
        [str(Path(sys.argv[1]).resolve()), '--service', '--spirv-cross', str(Path(sys.argv[2]).resolve()),
         '--out', str(out), '--chain', 'mkw-sm86-v5-unverified'],
        input=(out / 'request.json').read_text(), text=True, capture_output=True,
        check=True, env=os.environ.copy(), timeout=30)
    reply = json.loads(result.stdout)
    if not reply.get('ok'):
        raise RuntimeError(reply)
    source = out / 'atomic-probe_b.metal'
    assert '.atomic_fetch_add(' in source.read_text()
    probe = out / 'check'
    subprocess.run(['clang', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                    str(here / 'atomic_probe.m'), '-o', str(probe)], check=True, timeout=30)
    subprocess.run([str(probe), str(source)], check=True, timeout=10)
    # Verify that the grid read uses the canonical replay table on the GPU.
    for offset in (4, 0, 8, 12):
        subprocess.run([sys.executable, str(here / 'make_atomic_probe.py'), str(out), str(offset)], check=True)
        result = subprocess.run(
            [str(Path(sys.argv[1]).resolve()), '--service', '--spirv-cross', str(Path(sys.argv[2]).resolve()),
             '--out', str(out), '--chain', 'mkw-sm86-v5-unverified'],
            input=(out / 'request.json').read_text(), text=True, capture_output=True,
            check=True, env=os.environ.copy(), timeout=30)
        assert json.loads(result.stdout).get('ok'), result.stdout
        reflection = json.loads((out / 'atomic-probe_b.json').read_text())
        if offset in (4, 8):
            assert reflection['drv_unfillable'] == [], reflection
            subprocess.run([str(probe), str(source), 'grid' if offset == 4 else 'grid-y'], check=True, timeout=10)
        else:
            assert reflection['drv_unfillable'] == ['c0+0x0'], reflection
    print('Unknown NVN2 header scalars remain rejected')
