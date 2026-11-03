"""lldb probe: who drives the sdk's Opus ENCODER at the post-audio park?

The headless boot stalls with the main thread hot in sdk's libopus encoder (ec_enc_icdf / ec_enc_done /
opus_fft_impl). This probe breaks at the encoder entry once and prints the full backtrace with both
module bases, so the frames can be attributed exactly.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/encode_probe.py" -o alcenc -o quit
"""
import lldb
import os
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcenc(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alcenc: arming")
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
    out(f"alcenc: main base = {main_base:#x}, sdk base = {sdk_base:#x}")
    if not main_base or not sdk_base:
        out("no bases"); return
    bp_seal.SetEnabled(False)
    bp = target.BreakpointCreateByAddress(sdk_base + 0xE0410)   # ec_enc_icdf entry
    bp.SetOneShot(True)
    out(f"bp at sdk+0xe0410: id={bp.GetID()}")

    for round_i in range(6):
        try:
            process.Continue()
            if process.GetState() != lldb.eStateStopped:
                out(f"state {process.GetState()}"); return
            th = process.GetSelectedThread()
            desc = th.GetStopDescription(160)
            if 'EXC_BAD_ACCESS' in desc:
                out(f"[SEGV] {desc}"); return
            if th.GetStopReason() != lldb.eStopReasonBreakpoint:
                out(f"[other] {desc}"); continue
            f = th.GetFrameAtIndex(0)
            r = {}
            for name in ('x0', 'x1', 'x2', 'x19', 'x20', 'x21'):
                v = f.FindRegister(name)
                r[name] = v.GetValueAsUnsigned() if v else 0
            out(f"[encode hit] x0={r['x0']:#x} x1={r['x1']:#x} x2={r['x2']:#x} " +
                f"x19={r['x19']:#x} x20={r['x20']:#x} x21={r['x21']:#x}")
            for i in range(14):
                fr = th.GetFrameAtIndex(i)
                pc = fr.GetPC()
                lr = fr.FindRegister('lr').GetValueAsUnsigned()
                def tag(v):
                    if main_base <= v < main_base + 0xCCF8000: return f"main+{v - main_base:#x}"
                    if sdk_base <= v < sdk_base + 0xCEC000: return f"sdk+{v - sdk_base:#x}"
                    return f"{v:#x}"
                out(f"  frame{i}: pc={tag(pc)} lr={tag(lr)}")
            return
        except Exception as e:
            out(f"round {round_i} failed: {e!r}"); return
    out("alcenc: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f encode_probe.alcenc alcenc')
