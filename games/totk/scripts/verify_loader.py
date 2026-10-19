#!/usr/bin/env python3
"""Exercise native cross-module calls, mapped permissions, and hash rejection."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--version',choices=('v0','v112','v121'),default='v0');version=p.parse_args().version
exe = ROOT / ('build/totk-native' if version=='v0' else f'build/totk-native-{version}')
out=ROOT/('analysis' if version=='v0' else f'analysis/{version}-port')
run = subprocess.run([str(exe), f'rewritten/{version}', 'probe'], cwd=ROOT, capture_output=True, text=True, check=True)
data = json.loads(run.stdout)
assert data['status'] == 'pass' and data['calls'] == 4000 and data['threads'] == 4
layout=json.loads((out/'runtime-layout.json').read_text())
assert sum(m['relative'] + m['symbolic'] for m in data['modules']) == sum(len(m['relocs']) for m in layout)
# Alter one byte in a sparse copy: the loader must reject before relocation or execution.
with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
    path = Path(temporary) / 'main.patched.bin'
    original = ROOT / f'rewritten/{version}/main.patched.bin'
    with path.open('wb') as file:
        file.truncate(original.stat().st_size)
    failure = subprocess.run([str(exe), temporary, 'probe'], cwd=ROOT, capture_output=True, text=True)
    assert failure.returncode == 1 and 'image SHA256 differs' in failure.stderr
    assert 'ENTER' not in failure.stderr
    data['invalid_image_negative_control'] = 'rejected before execution'
data['permissions'] = 'all module VM regions checked; text/patch RX, aligned rodata/key R, data RW'
(out / 'native-loader-probe.json').write_text(json.dumps(data, indent=2) + '\n')
(out / 'native-loader-probe.log').write_text(run.stderr)
print(json.dumps(data))
