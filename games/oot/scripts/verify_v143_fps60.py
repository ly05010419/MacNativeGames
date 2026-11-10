#!/usr/bin/env python3
"""Emulate the 1.4.3 adaptive present-interval controller (main+0x2db174) from the rewritten main image, with and
without the 60 FPS patch table in runtime/game_patches_v143.c, and check the intervals it picks.

Shipping code clamps the interval to 2..3 (30 / 20 fps). With the patch the controller must reach 1 (60 fps) under
light frame work, rise to 2 and 3 under heavier work, step back down when work is light again, honour a listener veto,
and never produce 0. The unpatched controller is the negative control: it must never go below 2.
Usage: .venv/bin/python scripts/verify_fps60_patch.py   (writes a JSON summary to stdout, exit 1 on failure)"""
import json, re, struct, sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

from argparse import Namespace
args=Namespace(version="v143");addr=lambda x:x
IMAGE = f'rewritten/{args.version}/main.patched.bin'
FN,FN_END=0x2db174,0x2db40c
TICK_GOT,WINDOW_FN_GOT=0x3acceb8,0x396e5d0
raw_image=open(IMAGE,'rb').read()
TICK_HZ = 19_200_000
STOP, STUBS, HEAP, STACK = 0x10000, 0x20000, 0x100000, 0x200000

def patch_table():
    src = open('runtime/game_patches_v143.c').read()
    src = re.sub(r'GAME_ADDR\((0x[0-9a-f]+)\)', r'\1', src)
    # only the 60 FPS controller table: game_patches.c also holds the real-time frame-step and render-resolution tables
    m = re.search(r'fps60\[[^\]]*\] = \{(.*?)\n\};', src, re.S)
    if not m: raise SystemExit('fps60_sites table not found in runtime/game_patches_v143.c')
    rows = re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', m.group(1))
    if len(rows) != 7: raise SystemExit('expected 7 rows in fps60_sites, found %d' % len(rows))
    return [(addr(int(a,16)),int(b,16),int(c,16)) for a,b,c in rows]

def function_bytes(patched, skip=()):
    img = open(IMAGE, 'rb').read()
    code = bytearray(img[FN:FN_END])
    for off, orig, repl in patch_table():
        have = struct.unpack_from('<I', code, off - FN)[0]
        if have != orig: raise SystemExit('main+%#x is %08x, expected %08x' % (off, have, orig))
        if patched and off not in skip: struct.pack_into('<I', code, off - FN, repl)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = False
    for ins in md.disasm(bytes(code), FN):   # every direct branch must stay inside the function
        if ins.mnemonic in ('b', 'bl') or ins.mnemonic.startswith(('b.', 'cb', 'tb')):
            tgt = int(ins.op_str.split('#')[-1], 16)
            if not FN <= tgt < FN_END: raise SystemExit('branch at %#x leaves the controller: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    return bytes(code)

ASM = {   # tiny stub routines (pre-assembled AArch64)
    'ret': [0xd65f03c0],
    'record_w1': [0xd2a00048, 0xb9000101, 0xd65f03c0],            # movz x8,#0x2,lsl#16 (0x20000); str w1,[x8]; ret
    'veto': [0x52800028, 0x39006028, 0xd65f03c0],                   # mov w8,#1; strb w8,[x1,#0x10]; ret
}

class Controller:
    def __init__(self, patched, cand, cooldown, current=2, bias=0.0, threshold=3, cd_init=30, listener=None, skip=()):
        mu = self.mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.mem_map(FN & ~0xfff, 0x2000); mu.mem_write(FN, function_bytes(patched, skip))
        mu.mem_map(STOP, 0x1000); mu.mem_write(STOP, struct.pack('<I', 0xd4200000))   # brk #0
        mu.mem_map(STUBS, 0x1000)
        stub = {}
        for i, (name, words) in enumerate(ASM.items()):
            stub[name] = STUBS + 0x100 + 0x40 * i; mu.mem_write(stub[name], struct.pack('<%dI' % len(words), *words))
        mu.mem_map(TICK_GOT & ~0xfff, 0x1000); mu.mem_map(WINDOW_FN_GOT & ~0xfff, 0x1000)
        mu.mem_map(HEAP, 0x10000); mu.mem_map(STACK, 0x10000)
        mu.mem_write(HEAP + 0x8000, struct.pack('<Q', TICK_HZ)); mu.mem_write(TICK_GOT, struct.pack('<Q', TICK_HZ))
        mu.mem_write(HEAP + 0x8010, struct.pack('<Q', stub['record_w1'])); mu.mem_write(WINDOW_FN_GOT, struct.pack('<Q', HEAP + 0x8010))
        self.sys=HEAP;self.ctrl=HEAP+0x200;holder=HEAP+0x4000
        mu.mem_write(self.sys+0x150,struct.pack('<Q',holder));mu.mem_write(holder+0x70,struct.pack('<Q',0x7000));mu.mem_write(holder+0xa8,b'\xff')
        mu.mem_write(self.ctrl,struct.pack('<IIffII',2,3,bias,bias,threshold,cd_init))
        mu.mem_write(self.ctrl+0x28,struct.pack('<I',60))
        mu.mem_write(self.ctrl+0x40,struct.pack('<IIIIII',current,cand,0,cooldown,60//current,60//current))
        sentinel=self.ctrl+0x68
        mu.mem_write(0x20000,struct.pack('<I',0xffffffff))
        if listener:
            node, obj, vt = HEAP + 0x5000, HEAP + 0x5100, HEAP + 0x5200
            mu.mem_write(node + 8, struct.pack('<QQ', sentinel, obj))
            mu.mem_write(obj + 0x28, struct.pack('<Q', vt)); mu.mem_write(vt, struct.pack('<Q', stub[listener]))
            mu.mem_write(sentinel, struct.pack('<QQ', node, node))
        else:
            mu.mem_write(sentinel, struct.pack('<QQ', sentinel, sentinel))
        self.listener = listener

    def frame(self, work_ms, gpu_ms=None):
        mu = self.mu
        mu.mem_write(self.sys+0x118,struct.pack('<qqq',1,int(work_ms*TICK_HZ/1000),int((work_ms if gpu_ms is None else gpu_ms)*TICK_HZ/1000)))
        mu.reg_write(UC_ARM64_REG_X0, self.ctrl); mu.reg_write(UC_ARM64_REG_X1, self.sys)
        mu.reg_write(UC_ARM64_REG_SP, STACK + 0x8000); mu.reg_write(UC_ARM64_REG_LR, STOP)
        mu.emu_start(FN, STOP, count=4000)
        if mu.reg_read(UC_ARM64_REG_PC) != STOP: raise SystemExit('controller did not return')
        cur = struct.unpack('<I', mu.mem_read(self.ctrl + 0x40, 4))[0]
        sys88 = struct.unpack('<I', mu.mem_read(self.sys + 0x88, 4))[0]
        window = struct.unpack('<I', mu.mem_read(0x20000, 4))[0]
        if not (cur == sys88 == window): raise SystemExit('interval copies disagree: ctrl %d sys %d window %d' % (cur, sys88, window))
        return cur

def run(ctrl, plan):
    out = []
    for ms, n in plan:
        out += [ctrl.frame(ms) for _ in range(n)]
    return out

LIGHT, MEDIUM, HEAVY = 8.0, 25.0, 40.0
results, failures = [], []
def check(name, ok, detail):
    results.append({'case': name, 'pass': bool(ok), 'detail': detail})
    if not ok: failures.append(name)

for cand in (0, 2, 3):
    for cooldown in (0, 30):
        for listener in (None, 'ret'):
            tag = 'cand=%d cooldown=%d listener=%s' % (cand, cooldown, listener or 'none')
            seq = run(Controller(True, cand, cooldown, listener=listener), [(LIGHT, 200), (MEDIUM, 60), (HEAVY, 60), (LIGHT, 300)])
            first1 = seq.index(1) if 1 in seq else None
            check('patched ' + tag + ': never 0', 0 not in seq, 'min %d' % min(seq))
            check('patched ' + tag + ': light reaches 1', first1 is not None and all(v == 1 for v in seq[first1:200]), 'first at frame %s' % first1)
            check('patched ' + tag + ': medium rises to 2', seq[259] == 2, 'frame 259 = %d' % seq[259])
            check('patched ' + tag + ': heavy rises to 3', seq[319] == 3, 'frame 319 = %d' % seq[319])
            check('patched ' + tag + ': light steps back to 1', seq[-1] == 1, 'last = %d, intervals seen after heavy: %s' % (seq[-1], sorted(set(seq[320:]))))
            base = run(Controller(False, max(cand, 2), cooldown, listener=listener), [(LIGHT, 200), (MEDIUM, 60), (HEAVY, 60), (LIGHT, 300)])
            check('unpatched ' + tag + ': never below 2 (negative control)', min(base) >= 2, 'min %d' % min(base))

no_trigger = run(Controller(True, 2, 0, listener='ret', skip={0x2db22c}), [(LIGHT, 300)])
check('negative control: without the lower-trigger site a stored candidate of 2 never reaches 1', 1 not in no_trigger, 'seen %s' % sorted(set(no_trigger)))
veto = run(Controller(True, 2, 0, listener='veto'), [(LIGHT, 300)])
check('patched with a vetoing listener stays at 2', set(veto) == {2}, 'seen %s' % sorted(set(veto)))
biased = run(Controller(True, 2, 0, bias=0.5), [(LIGHT, 200), (12.0, 200)])
check('bias 0.5: 8 ms frames reach 1, 12 ms frames go back to 2', 1 in biased[:200] and biased[-1] == 2, 'last %d' % biased[-1])

gpu=Controller(True,2,0)
seq=[gpu.frame(8.0,40.0) for _ in range(120)]
check('GPU work dominates light CPU work',seq[-1]==3,'last %d'%seq[-1])
cpu=Controller(True,2,0)
seq=[cpu.frame(40.0,8.0) for _ in range(120)]
check('CPU work dominates light GPU work',seq[-1]==3,'last %d'%seq[-1])
print(json.dumps({'status': 'fail' if failures else 'pass', 'checked': len(results), 'failures': failures,
                  'version': args.version, 'scope': 'present controller emulated from the rewritten image; patch rows parsed from runtime/game_patches_v143.c',
                  'results': results}, ensure_ascii=False, indent=1))
sys.exit(1 if failures else 0)
