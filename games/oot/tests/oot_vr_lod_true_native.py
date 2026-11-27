"""Run the depth sums' veneers (zeldaVR/runtime/oot_vr_lod_true.h) on the shared hook body natively on arm64 macOS.

Usage: python3 games/oot/tests/oot_vr_lod_true_native.py --work-dir <external-directory>
No game files are required. The body's and the veneers' own words run; only their ADRP to the data page is replaced by the
assembler's (the page's address is not known before linking). A "site" is `bl <veneer>` with the registers loaded before it.
"""
from pathlib import Path
import argparse
import platform
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--work-dir', required=True, type=Path)
args = parser.parse_args()
if platform.system() != 'Darwin' or platform.machine() != 'arm64':
    parser.error('requires arm64 macOS')
work = args.work_dir.resolve()
work.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parent / 'oot_vr_lod_true_native.c'

subprocess.run(['xcrun', 'clang', '-DGEN', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(work / 'lod-gen')], check=True)
words = {}
for line in subprocess.run([str(work / 'lod-gen')], check=True, capture_output=True, text=True).stdout.splitlines():
    name, *rest = line.split()
    words[name] = [int(w, 16) for w in rest]
assert len(words['body']) == 90 and len(words['veneer0']) == len(words['veneer1']) == 12
BODY_ADRP, VENEER_ADRP = 38, 2

asm = '''.data
.p2align 12
.globl _hk_page
_hk_page: .space 4096
.p2align 4
_lt_save: .space 32
.text
.p2align 2
L_body:
'''
for i, w in enumerate(words['body']):
    asm += ' adrp x16, _hk_page@PAGE\n' if i == BODY_ADRP else f' .long 0x{w:08x}\n'
for d in range(2):
    asm += f'.globl _lt_ven{d}\n_lt_ven{d}:\n'
    for i, w in enumerate(words[f'veneer{d}']):
        asm += ' adrp x16, _hk_page@PAGE\n' if i == VENEER_ADRP else f' .long 0x{w:08x}\n'

for d in range(2):
    asm += f'''.globl _lt_run{d}
_lt_run{d}:
 stp x29, x30, [sp, #-176]!
 mov x29, sp
'''
    asm += ''.join(f' stp x{i}, x{i + 1}, [sp, #{16 + 8 * (i - 19)}]\n' for i in range(19, 29, 2))
    asm += ''.join(f' stp d{i}, d{i + 1}, [sp, #{96 + 8 * (i - 8)}]\n' for i in range(8, 16, 2))
    asm += ''' adrp x9, _lt_save@PAGE
 add x9, x9, _lt_save@PAGEOFF
 mov x10, sp
 stp x1, x10, [x9]
 mov x10, x0
'''
    asm += ''.join(f' ldr q{r}, [x10, #{256 + 16 * r}]\n' for r in range(32))
    asm += ''.join(f' ldp x{i}, x{i + 1}, [x10, #{8 * i}]\n' for i in range(0, 10, 2))
    asm += ''.join(f' ldp x{i}, x{i + 1}, [x10, #{8 * i}]\n' for i in range(12, 30, 2))
    asm += ' ldp x10, x11, [x10, #80]\n'
    asm += f' bl _lt_ven{d}\n.globl _lt_after{d}\n_lt_after{d}:\n'
    asm += ''' stp x0, x1, [sp, #-16]!
 adrp x0, _lt_save@PAGE
 add x0, x0, _lt_save@PAGEOFF
 ldr x0, [x0]
'''
    asm += ''.join(f' stp x{i}, x{i + 1}, [x0, #{8 * i}]\n' for i in range(2, 30, 2))
    asm += ' str x30, [x0, #240]\n'
    asm += ''.join(f' str q{r}, [x0, #{256 + 16 * r}]\n' for r in range(32))
    asm += ''' ldp x2, x3, [sp], #16
 stp x2, x3, [x0]
 mov x10, sp
 adrp x9, _lt_save@PAGE
 add x9, x9, _lt_save@PAGEOFF
 ldr x11, [x9, #8]
 add x12, x0, #784
 stp x10, x11, [x12]
 mov sp, x11
'''
    asm += ''.join(f' ldp d{i}, d{i + 1}, [sp, #{96 + 8 * (i - 8)}]\n' for i in range(8, 16, 2))
    asm += ''.join(f' ldp x{i}, x{i + 1}, [sp, #{16 + 8 * (i - 19)}]\n' for i in range(19, 29, 2))
    asm += ' ldp x29, x30, [sp], #176\n ret\n'

(work / 'lod.s').write_text(asm, encoding='utf-8')
subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', str(source), str(work / 'lod.s'), '-o', str(work / 'lod-run')], check=True)
subprocess.run([str(work / 'lod-run')], check=True)
