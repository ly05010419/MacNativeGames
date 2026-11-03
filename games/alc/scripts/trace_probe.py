"""lldb probe: full ordered trace of the shader-init path that fails.

Non-one-shot breakpoints on entries and fail targets; every stop is logged in order, so one boot shows
the whole flow: which sub-init runs, which fails, whether the failure point moves between runs.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/trace_probe.py" -o alctrace -o quit
"""
import lldb
import os
import sys
import time

# entries and fail targets of the failing region (main-relative)
WATCH = (
    (0x40d5a0, 'shader_init'),
    (0x40dfd0, 'sub1_nvn'),
    (0x40f8e0, 'swapchain'),
    (0x40fa28, 'swapchain_FAIL'),
    (0x40e14c, 'sub1_FAIL'),
    (0x40e180, 'sub2'),
    (0x40e2e0, 'sub3'),
    (0x9dea90, 'nvn_init_import'),
    (0x40df98, 'FAIL_98'),
    (0x40dfa8, 'FAIL_a8'),
    (0x40dfbc, 'FAIL_bc'),
    (0x3e3914, 'nnmain_fail_exit'),
    (0x9c0cd0, 'walker'),
)


def out(msg):
    print(msg)
    sys.stdout.flush()


def alctrace(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alctrace: arming")
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
    out(f"alctrace: main base = {base:#x}")
    if not base:
        out("no base"); return
    bp_seal.SetEnabled(False)

    names = {}
    for off, name in WATCH:
        bp = target.BreakpointCreateByAddress(base + off)
        bp.SetOneShot(False)
        names[base + off] = name

    seen = {}
    for round_i in range(80):
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
        name = names.get(r['pc'])
        if name is None:
            out(f"[?] main+{r['pc'] - base:#x} ({desc})")
            continue
        seen[name] = seen.get(name, 0) + 1
        out(f"[{name}#{seen[name]}] lr=main+{r['lr'] - base:#x} x0={r['x0']:#x} x1={r['x1']:#x} " +
            f"x2={r['x2']:#x} x3={r['x3']:#x}")
    out("alctrace: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f trace_probe.alctrace alctrace')
