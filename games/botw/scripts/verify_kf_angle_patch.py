#!/usr/bin/env python3
"""Compile and execute the moving-platform veneer natively on Apple silicon.

Instruction words come from patch_kf_angle; its external call links to the host
atan2f. No game data, interpreter or CPU emulator is required. An optional original
main image also checks patch guards and padding: verify_kf_angle_patch.py [image].
Build products go to TMPDIR and are removed after the check.
"""
import argparse
import json
import platform
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / 'totk'
STUB = ((0x15a1c64 + 15) & ~15) + 0x300
SITE, ATAN2F = 0x12d57fc, 0x159ff10


def c_tables():
    source = (ROOT / 'runtime/game_patches_botw.c').read_text()
    body = source.split('static void patch_kf_angle(', 1)[1].split('\n}\n', 1)[0]
    guards = body.split('check[] = {', 1)[1].split('};', 1)[0]
    checks = {int(a, 16): int(b, 16) for a, b in
              re.findall(r'\{0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u\}', guards)}
    if len(checks) != 9:
        raise ValueError('unrecognised patch guard table')
    words, assembly = [], []
    for line in body.split('veneer[] = {', 1)[1].split('};', 1)[0].splitlines():
        match = re.match(r'\s*(0x[0-9a-f]{8})u(\s*\|)?', line)
        if not match:
            if line.strip():
                raise ValueError('unrecognised veneer instruction: ' + line)
            continue
        word = int(match[1], 16)
        if match[2]:
            if word != 0x94000000 or 'ATAN2F' not in line:
                raise ValueError('unexpected external veneer call')
            word |= ((ATAN2F - STUB - 4 * len(words)) >> 2) & 0x3ffffff
            assembly.append('bl _atan2f')
        else:
            assembly.append(f'.long {word:#x}')
        words.append(word)
    if len(words) != 14 or assembly.count('bl _atan2f') != 1:
        raise ValueError('unexpected veneer layout')
    return checks, words, assembly


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', nargs='?', type=Path)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('this probe executes native ARM64 instructions on Apple silicon')
    checks, words, assembly = c_tables()
    if args.image:
        image = args.image.read_bytes()
        for off, expected in checks.items():
            have, = struct.unpack_from('<I', image, off)
            if have != expected:
                raise ValueError(f'original instruction mismatch at main+{off:#x}: {have:08x} != {expected:08x}')
        if any(image[STUB:STUB + 4 * len(words)]):
            raise ValueError('veneer padding is not empty')
    branch = 0x94000000 | (((STUB - SITE) >> 2) & 0x3ffffff)
    displacement = branch & 0x3ffffff
    if displacement & (1 << 25):
        displacement -= 1 << 26
    if SITE + 4 * displacement != STUB:
        raise ValueError('patch call is outside the ARM64 branch range')
    with tempfile.TemporaryDirectory(prefix='botw-angle-') as tmp:
        work = Path(tmp)
        source = work / 'angle.S'
        source.write_text('.text\n.p2align 2\n.globl _kf_angle\n_kf_angle:\n' + '\n'.join(assembly) + '\n')
        executable = work / 'angle-probe'
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(source),
                        str(Path(__file__).with_name('kf_angle_probe.c')), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or f'native probe exit {result.returncode}')
        report = json.loads(result.stdout)
        report.update(image=str(args.image) if args.image else None, guarded_words=len(checks), veneer=hex(STUB))
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
