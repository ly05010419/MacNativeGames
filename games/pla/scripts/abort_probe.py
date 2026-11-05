"""lldb probe: what does nn::diag::detail::AbortImpl get called with?

One-shot breakpoint at sdk's AbortImpl entry; at the hit, dump the file/func/expr/line arguments.
Continues afterwards (the SDK Break will still fire and stop the boot loudly).

Run (from the work folder):
  lldb -b build/pla-native -o "command script import ../scripts/abort_probe.py" -o plaabort -o quit
"""
import lldb
import os
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def plaabort(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("plaabort: arming AbortImpl probe")
    si = lldb.SBLaunchInfo(['rewritten/v1101', 'boot'])
    si.SetWorkingDirectory(os.environ.get('PLA_BUILD', os.getcwd()))
    si.SetEnvironmentEntries(['TOTK_HEADLESS=1', 'PLA_LOG=/tmp/pla-abort-probe.log'], True)
    err = lldb.SBError()
    process = target.Launch(si, err)
    if not process or not process.IsValid():
        out(f"launch failed: {err}"); return
    waited = 0
    while process.GetState() in (lldb.eStateRunning, lldb.eStateLaunching):
        time.sleep(0.1); waited += 0.1
        if waited > 60: break
    frame = process.GetSelectedThread().GetFrameAtIndex(0)
    sdk = int(frame.EvaluateExpression('(uint8_t*)modules[2].base').GetValueAsUnsigned() or 0)
    out(f"plaabort: sdk base = {sdk:#x}")
    if not sdk:
        out("no sdk base"); return
    bp = target.BreakpointCreateByAddress(sdk + 0x145170)   # nn::diag::detail::AbortImpl entry
    bp.SetOneShot(False)
    out(f"bp AbortImpl sdk+0x145170: id={bp.GetID()}")
    for round_i in range(6):
        process.Continue()
        if process.GetState() != lldb.eStateStopped:
            out(f"state {process.GetState()}"); return
        th = process.GetSelectedThread()
        f = th.GetFrameAtIndex(0)
        r = {}
        for name in ('x0', 'x1', 'x2', 'x3', 'x4', 'lr'):
            v = f.FindRegister(name)
            r[name] = v.GetValueAsUnsigned() if v else 0
        out(f"--- hit #{round_i} AbortImpl")
        for name in ('x0', 'x1', 'x2'):
            p = r[name]
            if p:
                s = f.EvaluateExpression(f'(const char*){p}').GetSummary()
                out(f"  {name} = {p:#x} {s}")
        out(f"  x3 = {r['x3']:#x}  x4 = {r['x4']:#x}  lr = sdk+{r['lr']-sdk:#x}" if r['lr'] > sdk else f"  x3 = {r['x3']:#x}  lr = {r['lr']:#x}")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f abort_probe.plaabort plaabort')
