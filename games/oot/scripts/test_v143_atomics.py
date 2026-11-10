#!/usr/bin/env python3
"""Verify generated 1.4.3 cold CAS retries on this Mac, including a broken control."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/d2r'))
import nsoimg


def main():
    out = ROOT / 'analysis/v143-port'; out.mkdir(parents=True, exist_ok=True)
    source, executable = ROOT / 'scripts/native_exclusive_probe.c', ROOT / 'build/native_exclusive_probe'
    subprocess.run(['xcrun','clang','-std=c11','-O2','-Wall','-Wextra','-Werror',str(source),'-o',str(executable)], check=True)
    header = nsoimg.header(ROOT / 'rewritten/v143', 'main')
    command = [str(executable), str(ROOT / 'rewritten/v143/main.patched.bin'),
               hex(header['KEY_PAGE_OFF']), str(header['KEY_K18_IDX'])]
    positive = subprocess.run(command, capture_output=True, text=True, timeout=35)
    (out / 'native-exclusive.log').write_text(positive.stdout + positive.stderr)
    assert positive.returncode == 0, 'Native atomic probe failed; see native-exclusive.log'
    negative = subprocess.run(command + ['negative'], capture_output=True, text=True, timeout=35)
    (out / 'native-exclusive-negative.log').write_text(negative.stdout + negative.stderr)
    assert negative.returncode != 0 and 'Assertion failed' in negative.stderr, 'Broken increment was not rejected'
    report = dict(status='pass', cases=[json.loads(line) for line in positive.stdout.splitlines()],
                  negative_controls_detected=1, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  scope='Exact generated cold-retry code on this Darwin arm64 CPU; 8 threads; no game or GPU executed')
    print(json.dumps(report))


if __name__ == '__main__': main()
