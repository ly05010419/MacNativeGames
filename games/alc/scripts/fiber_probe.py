"""lldb probe: the fiber/phase park, seen in order.

Breakpoints: sdk SwitchToFiber entry, the nnMain spin (main+0x3e415c), and the completion callback
(main+0x3e41b0). Every hit is logged with the flag value, so one headless boot shows whether the fiber
system runs and where the resume is lost.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/fiber_probe.py" -o alcfiber -o quit
"""
import lldb
import os
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcfiber(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alcfiber: arming")
    bp_seal = target.BreakpointCreateByName('seal')
    si = lldb.SBLaunchInfo(['rewritten/v101', 'boot'])
    si.SetWorkingDirectory(os.environ.get('ALC_BUILD', os.getcwd()))   # run from AC_build, or name it in ALC_BUILD
    si.SetEnvironmentEntries(['TOTK_HEADLESS=1'], True)
    err = lldb.SBError()
    process = target.Launch(si, err)
    if not process or not process.IsValid():
        out(f"launch failed: {err}"); return
    waited = 0
    while process.GetState() in (lldb.eStateRunning, lldb.eStateLaunching, lldb.eStateAttaching):
        time.sleep(0.1); waited += 0.1
        if waited > 60: break
    frame = process.GetSelectedThread().GetFrameAtIndex(0)
    main_base = int(frame.EvaluateExpression('(unsigned long)modules[0].base').GetValueAsUnsigned() or 0)
    sdk_base = int(frame.EvaluateExpression('(unsigned long)modules[1].base').GetValueAsUnsigned() or 0)
    out(f"alcfiber: main base = {main_base:#x}, sdk base = {sdk_base:#x}")
    if not main_base or not sdk_base:
        out("no bases"); return
    bp_seal.SetEnabled(False)

    obj_v = frame.EvaluateExpression('*(unsigned long *)(modules[0].base + 0xdb84c8)')
    obj = int(obj_v.GetValueAsUnsigned())
    flag = obj + 0x194
    out(f"flag at {flag:#x}")

    at = {}
    for off, name in ((0x3e415c, 'spin'), (0x3e41b0, 'complete'), (0x3e4240, 'fiber_body'), (0x40d1b0, 'fiber_shader'), (0x3c1500, 'fiber_step1'), (0x9e3e80, 'fiber_step2'), (0x6e5310, 'fiber_tail')):
        b = target.BreakpointCreateByAddress(main_base + off)
        b.SetOneShot(False)
        at[main_base + off] = name
        out(f"bp {name}: id={b.GetID()}")
    for soff, sname in ((0x318760, 'SwitchToFiber'), (0x3185e0, 'InitializeFiber'),
                        (0x320d84, 'CallFiberFunction'), (0x5125c0, 'switch_prim')):
        sb = target.BreakpointCreateByAddress(sdk_base + soff)
        at[sdk_base + soff] = sname

    seen = {}
    for round_i in range(40):
        try:
            process.Continue()
            if process.GetState() != lldb.eStateStopped:
                out(f"state {process.GetState()}"); return
            th = process.GetSelectedThread()
            desc = th.GetStopDescription(160)
            if 'EXC_BAD_ACCESS' in desc:
                out(f"[SEGV] {desc}"); return
            f = th.GetFrameAtIndex(0)
            pc = f.FindRegister('pc').GetValueAsUnsigned()
            name = at.get(pc)
            if name is None:
                out(f"[?] {pc:#x} ({desc})"); continue
            seen[name] = seen.get(name, 0) + 1
            data = process.ReadMemory(flag, 4, err)
            cur = struct.unpack('<I', data)[0] if data else -1
            x0 = f.FindRegister('x0').GetValueAsUnsigned()
            out(f"[{name}#{seen[name]}] x0={x0:#x} flag={cur:#x} lr={f.FindRegister('lr').GetValueAsUnsigned() - main_base:#x}(main-rel)")
            if name == 'spin' and seen[name] >= 2:
                out("alcfiber: spin reached, done"); return
            if name == 'CallFiberFunction' and seen[name] >= 3:
                out("alcfiber: fiber entry seen 3 times, done"); return
        except Exception as e:
            out(f"round {round_i} failed: {e!r}"); return
    out("alcfiber: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f fiber_probe.alcfiber alcfiber')
