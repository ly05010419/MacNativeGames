#!/usr/bin/env python3
"""Check the local 1.4.3 EASU shader's RGB/HDR contract on the Mac GPU."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
KEY = 'ac58fc18eda5f3878b16074dc28cd441644a66710f1b73954d39e57ac357561b-86e45adffa96'


def main():
    os.chdir(ROOT)
    cache = ROOT / 'build/shader-cache/rt-totk-native-v6-unverified'
    code = cache / (KEY + '.code')
    assert hashlib.sha256(code.read_bytes()).hexdigest() == 'a8835fa299e9033df670842ad00a1799353cd504817caf06f171152bee67f2b3'
    reflection = json.loads((cache / (KEY + '_b.json')).read_text())
    assert bytes.fromhex(reflection['c1'])[:8].hex() == '3946bc1f00000040'
    source = cache / (KEY + '_b.metal')
    out = ROOT / 'analysis/v143-port/upscale-native'
    out.mkdir(exist_ok=True)
    subprocess.run([
        'xcrun', 'clang', '-fobjc-arc', '-O1', '-Wall', '-Wextra', '-Werror',
        'scripts/upscale_color_probe.m', '-framework', 'Metal', '-framework', 'Foundation',
        '-o', 'build/upscale-color-probe',
    ], check=True)
    command = ['python3', 'scripts/dev/game_slot.py', 'run', '--purpose',
               'v143 exact EASU RGB/HDR GPU validation', '--expect-seconds', '15', '--',
               'build/upscale-color-probe', str(source)]
    run = subprocess.run(command, capture_output=True, text=True, check=True)
    (out / 'gpu.log').write_text(run.stdout + run.stderr)
    report = json.loads(run.stdout)
    negative = subprocess.run(command, capture_output=True, text=True,
                              env=os.environ | {'TOTK_PROBE_NEGATIVE': '1'})
    (out / 'gpu-negative.log').write_text(negative.stdout + negative.stderr)
    assert negative.returncode == 1 and 'got 0.0285711 want 0.1' in negative.stderr
    report.update(negative_control='missing output gain rejected',
                  code_sha256=hashlib.sha256(code.read_bytes()).hexdigest(),
                  translated_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  probe_source_sha256=hashlib.sha256((ROOT / 'scripts/upscale_color_probe.m').read_bytes()).hexdigest())
    (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
