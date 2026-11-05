"""lldb probe: which guarded call inside main+0x40d5a0 fails?

One-shot breakpoints on the function's four fail targets; at a hit, lr names the failing bl, and the
argument registers are dumped. Continues to the SEGV afterwards.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/fail_probe.py" -o alcfail -o quit
"""
import lldb
import os
import sys
import time

FAIL_TARGETS = (0x40df98, 0x40dfa8, 0x40dfbc, 0x40df98)


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcfail(debugger, command, exe_ctx, result, _dict):
    targets = tuple(int(t, 16) for t in command.split()) or FAIL_TARGETS
    target = debugger.GetSelectedTarget()
    out(f"alcfail: arming (fail targets: {[f'{t:#x}' for t in targets]})")
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
    out(f"alcfail: main base = {base:#x}")
    if not base:
        out("no base"); return
    bp_seal.SetEnabled(False)

    for off in set(targets):
        bp = target.BreakpointCreateByAddress(base + off)
        bp.SetOneShot(True)
        out(f"bp fail_target main+{off:#x}: id={bp.GetID()}")

    for round_i in range(30):
        process.Continue()
        if process.GetState() != lldb.eStateStopped:
            out(f"state {process.GetState()}"); return
        th = process.GetSelectedThread()
        desc = th.GetStopDescription(160)
        f = th.GetFrameAtIndex(0)
        r = {}
        for name in ('x0', 'x1', 'x2', 'x3', 'pc', 'lr'):
            v = f.FindRegister(name)
            r[name] = v.GetValueAsUnsigned() if v else 0
        if 'EXC_BAD_ACCESS' in desc or th.GetStopReason() == lldb.eStopReasonSignal:
            out(f"[SEGV] {desc} pc=main+{r['pc'] - base:#x} lr=main+{r['lr'] - base:#x}")
            return
        rel = r['pc'] - base
        if rel in targets:
            bl = r['lr'] - 4 - base
            out(f"[FAIL] hit main+{rel:#x}: failing call at main+{bl:#x} " +
                f"x0={r['x0']:#x} x1={r['x1']:#x} x2={r['x2']:#x} x3={r['x3']:#x}")
            return
        out(f"[other] main+{rel:#x} ({desc}) lr=main+{r['lr'] - base:#x}")
    out("alcfail: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f fail_probe.alcfail alcfail')
