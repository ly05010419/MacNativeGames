"""Run the actual time-step veneer words natively on arm64 macOS.

Usage: python3 games/oot/tests/oot_timestep_native.py --work-dir <external-directory>
No game files are required.
"""
from pathlib import Path
import re,subprocess,argparse,platform
parser=argparse.ArgumentParser();parser.add_argument('--work-dir',required=True,type=Path);args=parser.parse_args()
if platform.system()!='Darwin' or platform.machine()!='arm64':parser.error('requires arm64 macOS')
D=args.work_dir.resolve();D.mkdir(parents=True,exist_ok=True)
source=Path(__file__).resolve().parent
h=(source.parent/'runtime/oot_timestep.h').read_text()
asm='.data\n.p2align 12\n.globl _step_state\n_step_state: .space 4096\n.globl _step_check\n_step_check: .space 64\n.text\n'
for name,ret in [('input',21),('logic',30)]:
 vals=[int(x,16)for x in re.findall(r'0x[0-9a-f]+',re.search('uint32_t '+name+r'\[\] = \{(.*?)\};',h,re.S).group(1))]
 asm+=f'''.globl _call_{name}
_call_{name}:
 stp x29,x30,[sp,#-48]!
 stp x19,x21,[sp,#16]
 str d8,[sp,#32]
 mov x19,x0
 mov x21,x0
 mov x16,#0x1234
 mov x17,#0x5678
 movi v16.16b,#0x5a
 movi v17.16b,#0xa5
 cmp xzr,xzr
 bl L_{name}
 adrp x9,_step_check@PAGE
 add x9,x9,_step_check@PAGEOFF
 stp x16,x17,[x9]
 mrs x10,NZCV
 str x10,[x9,#16]
 stp q16,q17,[x9,#32]
'''
 if name=='logic':asm+=' fmov s0,s8\n'
 asm+=' ldr d8,[sp,#32]\n ldp x19,x21,[sp,#16]\n ldp x29,x30,[sp],#48\n ret\n'
 asm+=f'L_{name}:\n'
 for i,v in enumerate(vals):
  if i==2:asm+=' adrp x17,_step_state@PAGE\n'
  elif i==3:asm+=' add x17,x17,_step_state@PAGEOFF\n'
  elif i==ret:asm+=' ret\n'
  else:asm+=f' .long 0x{v:08x}\n'
(D/'step-test.S').write_text(asm)
subprocess.run(['xcrun','clang','-Wall','-Wextra','-Werror',str(source/'oot_timestep_native.c'),str(D/'step-test.S'),'-o',str(D/'step-test')],check=True)
subprocess.run([str(D/'step-test')],check=True)
