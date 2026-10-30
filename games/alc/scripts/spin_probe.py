"""lldb probe: who writes the state object's +0x194 flag that nnMain's spin waits on?

The boot parks at main+0x3e415c spinning on [0xdb84c8]+0x194. This probe reads the object pointer at
seal, arms a write watchpoint on the flag, breaks once at the spin site, and logs every write (pc/lr as
main+offsets) until the spin is reached.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/spin_probe.py" -o alcspin -o quit
"""
import lldb
import os
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcspin(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alcspin: arming")
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
    base = int(frame.EvaluateExpression('(uint8_t*)modules[0].base').GetValueAsUnsigned() or 0)
    out(f"alcspin: main base = {base:#x}")
    if not base:
        out("no base"); return

    data = process.ReadMemory(base + 0xdb84c8, 8, err)
    if not data:
        out(f"object pointer read failed: {err}"); return
    obj = struct.unpack('<Q', data)[0]
    out(f"state object = {obj:#x} (main+{obj - base:#x})")
    flag = obj + 0x194
    try:
        bp_seal.SetEnabled(False)
        wp = target.WatchAddress(flag, 8, False, True, err)
        out(f"watchpoint on {flag:#x} (+0x194): valid={wp.IsValid()} err={err}")
        if not wp.IsValid():
            out("watch failed"); return
    except Exception as e:
        out(f"arm failed: {e!r}"); return
    spin_bp = target.BreakpointCreateByAddress(base + 0x3e415c)
    out(f"spin bp at main+0x3e415c: id={spin_bp.GetID()}")

    seen = {}
    for round_i in range(14):
        try:
            process.Continue()
            if process.GetState() != lldb.eStateStopped:
                out(f"state {process.GetState()}"); return
            th = process.GetSelectedThread()
            desc = th.GetStopDescription(160)
            f = th.GetFrameAtIndex(0)
            r = {}
            for name in ('x0', 'x1', 'x8', 'x19', 'x20', 'w9', 'pc', 'lr'):
                v = f.FindRegister(name)
                r[name] = v.GetValueAsUnsigned() if v else 0
            if 'EXC_BAD_ACCESS' in desc or th.GetStopReason() == lldb.eStopReasonSignal:
                out(f"[SEGV] {desc} pc=main+{r['pc'] - base:#x} lr=main+{r['lr'] - base:#x}")
                return
            val = process.ReadMemory(flag, 4, err)
            cur = struct.unpack('<I', val)[0] if val else -1
            if th.GetStopReason() == lldb.eStopReasonWatchpoint:
                key = r['pc'] - base
                seen[key] = seen.get(key, 0) + 1
                out(f"[write#{seen[key]}] from main+{key:#x} lr=main+{r['lr'] - base:#x} " +
                    f"w0={r['x0'] & 0xffffffff:#x} x1={r['x1']:#x} x8={r['x8']:#x} x19={r['x19']:#x} " +
                    f"x20={r['x20']:#x} → flag now {cur:#x}")
            else:
                out(f"[spin reached] flag = {cur:#x} (pc=main+{r['pc'] - base:#x})")
                return
        except Exception as e:
            out(f"round {round_i} failed: {e!r}"); return
    out("alcspin: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f spin_probe.alcspin alcspin')
