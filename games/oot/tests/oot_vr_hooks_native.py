"""Run the shared OoT VR hook body and its stubs (runtime/oot_vr_hooks.h) natively on arm64 macOS.

Usage: python3 games/oot/tests/oot_vr_hooks_native.py --work-dir <external-directory>
No game files are required. The body's and the stubs' own words run; only the ADRP to the data page is replaced by the
assembler's (the page's address is not known before linking), and each stub is followed by one branch to our return label.
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
source = Path(__file__).resolve().parent / 'oot_vr_hooks_native.c'

subprocess.run(['xcrun', 'clang', '-DGEN', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(work / 'hooks-gen')], check=True)
words = {}
for line in subprocess.run([str(work / 'hooks-gen')], check=True, capture_output=True, text=True).stdout.splitlines():
    name, *rest = line.split()
    words[name] = [int(w, 16) for w in rest]
assert len(words['body']) == 90 and all(len(words[f'stub{k}']) == 5 for k in range(3))
ADRP_WORD = 38

asm = '''.data
.p2align 12
.globl _hk_page
_hk_page: .space 4096
.p2align 4
_hk_save: .space 64
.text
.p2align 2
L_body:
'''
for i, w in enumerate(words['body']):
    asm += ' adrp x16, _hk_page@PAGE\n' if i == ADRP_WORD else f' .long 0x{w:08x}\n'
for k in range(3):
    asm += f'L_stub{k}:\n' + ''.join(f' .long 0x{w:08x}\n' for w in words[f'stub{k}']) + f' b L_back{k}\n'

for k in range(3):
    asm += f'''.globl _run_site{k}
_run_site{k}:
 stp x29, x30, [sp, #-176]!
 mov x29, sp
'''
    asm += ''.join(f' stp x{i}, x{i + 1}, [sp, #{16 + 8 * (i - 19)}]\n' for i in range(19, 29, 2))
    asm += ''.join(f' stp d{i}, d{i + 1}, [sp, #{96 + 8 * (i - 8)}]\n' for i in range(8, 16, 2))
    asm += ''' adrp x9, _hk_save@PAGE
 add x9, x9, _hk_save@PAGEOFF
 mov x10, sp
 stp x1, x10, [x9]
 sub sp, sp, #64
 mov x10, sp
 str x10, [x9, #16]
 mov x10, x0
'''
    asm += ''.join(f' movi v{r}.16b, #0x{0x40 + r:02x}\n' for r in range(32))
    asm += ' mov x9, #0x60000000\n msr nzcv, x9\n'
    asm += ''.join(f' ldp x{i}, x{i + 1}, [x10, #{8 * i}]\n' for i in range(0, 10, 2))
    asm += ''.join(f' ldp x{i}, x{i + 1}, [x10, #{8 * i}]\n' for i in range(12, 30, 2))
    asm += ' ldr x30, [x10, #240]\n ldp x10, x11, [x10, #80]\n'
    asm += f' b L_stub{k}\nL_back{k}:\n'
    asm += ''' stp x0, x1, [sp, #-16]!
 adrp x0, _hk_save@PAGE
 add x0, x0, _hk_save@PAGEOFF
 ldr x0, [x0]
'''
    asm += ''.join(f' stp x{i}, x{i + 1}, [x0, #{8 * i}]\n' for i in range(2, 30, 2))
    asm += ' str x30, [x0, #240]\n mrs x9, nzcv\n str x9, [x0, #248]\n'
    asm += ''.join(f' str q{r}, [x0, #{256 + 16 * r}]\n' for r in range(32))
    asm += ''' ldp x2, x3, [sp], #16
 stp x2, x3, [x0]
 adrp x9, _hk_save@PAGE
 add x9, x9, _hk_save@PAGEOFF
 mov x10, sp
 ldr x11, [x9, #16]
 add x13, x0, #800
 stp x10, x11, [x13]
 ldr x12, [x11, #0x10]
 str x12, [x13, #16]
 ldr x10, [x9, #8]
 mov sp, x10
'''
    asm += ''.join(f' ldp d{i}, d{i + 1}, [sp, #{96 + 8 * (i - 8)}]\n' for i in range(8, 16, 2))
    asm += ''.join(f' ldp x{i}, x{i + 1}, [sp, #{16 + 8 * (i - 19)}]\n' for i in range(19, 29, 2))
    asm += ' ldp x29, x30, [sp], #176\n ret\n'

(work / 'hooks.s').write_text(asm, encoding='utf-8')
subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', str(source), str(work / 'hooks.s'), '-o', str(work / 'hooks-run')], check=True)
subprocess.run([str(work / 'hooks-run')], check=True)
