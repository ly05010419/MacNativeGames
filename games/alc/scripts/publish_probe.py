"""lldb probe: did nnMain's registry publish (main+0x3e37cc -> 0x9c1ab0 -> 0x9c0a10) succeed?

Breakpoints:
  0x9c1ab0  publish entry         — dump the stack temp (x0) fields
  0x9c0a10  insert entry          — dump reg (x0) and the guard [x0]
  0x9c1af0  after insert          — dump w0 (insert result) and reg +0x28..+0x48
  0x3e3914  big-init fail exit    — proves the init chain bailed
  0x9c0cd0  walker entry          — existing one-shot
then continues to the SEGV and dumps as before.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/publish_probe.py" -o alcpub -o quit
"""
import lldb
import os
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def read_qwords(process, addr, n, err):
    data = process.ReadMemory(addr, 8 * n, err)
    if not data:
        return None
    return struct.unpack(f'<{n}Q', data)


def alcpub(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alcpub: arming")
    bp_seal = target.BreakpointCreateByName('seal')
    si = lldb.SBLaunchInfo(['rewritten/v101', 'boot'])
    si.SetWorkingDirectory(os.environ.get('ALC_BUILD', os.getcwd()))   # run from AC_build, or name it in ALC_BUILD
    err = lldb.SBError()
    process = target.Launch(si, err)
    if not process or not process.IsValid():
        out(f"launch failed: {err}"); return
    waited = 0
    while process.GetState() in (lldb.eStateRunning, lldb.eStateLaunching, lldb.eStateAttaching):
        time.sleep(0.1); waited += 0.1
        if waited > 60: break
    frame = process.GetSelectedThread().GetFrameAtIndex(0)
    base = int(frame.EvaluateExpression('(uint8_t*)modules[0].base').GetValueAsUnsigned() or 0)
    out(f"alcpub: main base = {base:#x}")
    if not base:
        out("no base"); return
    bp_seal.SetEnabled(False)

    for off, name in ((0x9c1ab0, 'publish'), (0x9c0a10, 'insert'), (0x9c1af0, 'insert_ret'),
                      (0x3e3914, 'fail_exit'), (0x9c0cd0, 'walker')):
        bp = target.BreakpointCreateByAddress(base + off)
        bp.SetOneShot(name == 'walker')
        out(f"bp {name} @{base + off:#x}: id={bp.GetID()}")

    def regs():
        f = process.GetSelectedThread().GetFrameAtIndex(0)
        r = {}
        for name in ('x0', 'x1', 'x2', 'x3', 'x4', 'w0', 'pc', 'lr'):
            v = f.FindRegister(name)
            r[name] = v.GetValueAsUnsigned() if v else 0
        return r

    def dump_obj(tag, reg):
        vals = read_qwords(process, reg, 10, err)
        if vals:
            out(f"[{tag}] reg {reg:#x} (main+{reg - base:#x}): " +
                ' '.join(f"{v:#x}" for v in vals))
        else:
            out(f"[{tag}] object read failed: {err}")

    seen = {}
    for round_i in range(24):
        process.Continue()
        if process.GetState() != lldb.eStateStopped:
            out(f"state {process.GetState()}"); return
        th = process.GetSelectedThread()
        desc = th.GetStopDescription(160)
        if 'EXC_BAD_ACCESS' in desc or th.GetStopReason() == lldb.eStopReasonSignal:
            r = regs()
            out(f"[SEGV] {desc} pc=main+{r['pc'] - base:#x} lr=main+{r['lr'] - base:#x} " +
                ' '.join(f"{k}={v:#x}" for k, v in r.items()))
            dump_obj("SEGV", r['x23'] or base + 0xccb6298)
            return
        r = regs()
        # identify which breakpoint by pc
        names = {base + 0x9c1ab0: 'publish', base + 0x9c0a10: 'insert',
                 base + 0x9c1af0: 'insert_ret', base + 0x3e3914: 'fail_exit',
                 base + 0x9c0cd0: 'walker'}
        name = names.get(r['pc'], f"unknown@main+{r['pc'] - base:#x}")
        seen[name] = seen.get(name, 0) + 1
        tag = f"{name}#{seen[name]}"
        out(f"[{tag}] lr=main+{r['lr'] - base:#x} x0={r['x0']:#x} x1={r['x1']:#x} " +
            f"x2={r['x2']:#x} x3={r['x3']:#x} x4={r['x4']:#x} w0={r['w0'] & 0xffffffff:#x}")
        if name == 'insert':
            out(f"[{tag}] guard [reg] = {read_qwords(process, r['x0'], 1, err)[0]:#x}")
        if name in ('insert_ret', 'walker', 'fail_exit'):
            dump_obj(tag, base + 0xccb6298)
        if name == 'publish':
            vals = read_qwords(process, r['x0'], 8, err)
            if vals:
                out(f"[{tag}] temp {r['x0']:#x}: " + ' '.join(f"{v:#x}" for v in vals))
    out("alcpub: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f publish_probe.alcpub alcpub')
