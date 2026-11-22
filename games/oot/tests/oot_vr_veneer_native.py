"""Run the OoT VR veneers (runtime/oot_vr_veneer.h) natively on arm64 macOS.

Usage: python3 games/oot/tests/oot_vr_veneer_native.py --work-dir <external-directory>
No game files are required. The veneers' own words run between our entry and return labels; only the ADRP to the data
page is replaced by the assembler's (the page's address is not known before linking).
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
source = Path(__file__).resolve().parent / 'oot_vr_veneer_native.c'

subprocess.run(['xcrun', 'clang', '-DGEN', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(work / 'vr-gen')], check=True)
words = {}
for line in subprocess.run([str(work / 'vr-gen')], check=True, capture_output=True, text=True).stdout.splitlines():
    name, *rest = line.split()
    words[name] = [int(w, 16) for w in rest]
assert len(words['call']) == 63 and len(words['fov']) == 6

PATTERN_REGS = [r for r in range(32) if not 8 <= r < 16]


def veneer(name, adrp_index):
    out = []
    for i, w in enumerate(words[name]):
        out.append(' adrp x16, _vr_page@PAGE' if i == adrp_index else f' .long 0x{w:08x}')
    return '\n'.join(out) + '\n'


asm = '''.data
.p2align 12
.globl _vr_page
_vr_page: .space 4096
.globl _vr_out
.p2align 4
_vr_out: .space 1024
.globl _vr_seen
_vr_seen: .space 16
.text
.p2align 2
.globl _probe
_probe:
 adrp x9, _vr_seen@PAGE
 add x9, x9, _vr_seen@PAGEOFF
 stp x0, x1, [x9]
'''
asm += ''.join(f' mov x{r}, #0xdead\n' for r in range(18) if r != 16)   # x16 holds our address; clobbered below
asm += ' mov x16, #0xbeef\n'
asm += ''.join(f' movi v{r}.16b, #0xee\n' for r in PATTERN_REGS)
asm += ' mov x9, #0x90000000\n msr nzcv, x9\n ret\n'
asm += '''.globl _run_call
_run_call:
 stp x29, x30, [sp, #-48]!
 mov x29, sp
 stp x19, x20, [sp, #16]
 stp x21, x22, [sp, #32]
 mov x21, x0
 adrp x22, _vr_out@PAGE
 add x22, x22, _vr_out@PAGEOFF
 ldp x19, x20, [x21, #144]
'''
asm += ''.join(f' movi v{r}.16b, #0x{0x40 + r:02x}\n' for r in PATTERN_REGS)
asm += ' mov x9, #0x60000000\n msr nzcv, x9\n'
asm += ' mov x30, #0x3030\n movk x30, #0x3030, lsl #16\n movk x30, #0x3030, lsl #32\n movk x30, #0x3030, lsl #48\n'
asm += ''.join(f' ldp x{i}, x{i + 1}, [x21, #{8 * i}]\n' for i in range(2, 18, 2))
asm += ' ldp x0, x1, [x21]\n b L_call\nL_call_back:\n'
asm += ''.join(f' stp x{i}, x{i + 1}, [x22, #{8 * i}]\n' for i in range(0, 18, 2))
asm += ' mrs x9, nzcv\n stp x30, x9, [x22, #144]\n'
asm += ''.join(f' str q{r}, [x22, #{160 + 16 * k}]\n' for k, r in enumerate(PATTERN_REGS))
asm += ''' ldp x21, x22, [sp, #32]
 ldp x19, x20, [sp, #16]
 ldp x29, x30, [sp], #48
 ret
L_call:
'''
asm += veneer('call', 30)
asm += ''' b L_call_back
.globl _run_fov
_run_fov:
 stp x29, x30, [sp, #-32]!
 mov x29, sp
 str x19, [sp, #16]
 mov x19, x0
 b L_fov
L_fov_back:
 ldr x19, [sp, #16]
 ldp x29, x30, [sp], #32
 ret
L_fov:
'''
asm += veneer('fov', 1)
asm += ' b L_fov_back\n'
# The veneers branch back to the word after them: b L_call_back / b L_fov_back above sit right there.
(work / 'vr-veneer.S').write_text(asm)
subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', str(source), str(work / 'vr-veneer.S'),
                '-o', str(work / 'vr-veneer-test')], check=True)
subprocess.run([str(work / 'vr-veneer-test')], check=True)
