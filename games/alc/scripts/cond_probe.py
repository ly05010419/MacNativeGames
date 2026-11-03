"""lldb probe: conditional breakpoints on guarded sites to find the failing check.

Usage: alccond 9da628=x8==0 9da630=w8==0 ...   (main-relative hex addr = lldb condition)

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/cond_probe.py" \
       -o "alccond 9da628=x8==0 ..." -o quit
"""
import lldb
import os
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alccond(debugger, command, exe_ctx, result, _dict):
    sites = []
    for tok in command.split():
        if '=' in tok:
            addr, cond = tok.split('=', 1)
            sites.append((int(addr, 16), cond))
        else:
            sites.append((int(tok, 16), None))
    if not sites:
        out("alccond: no sites given"); return
    target = debugger.GetSelectedTarget()
    out(f"alccond: arming {len(sites)} sites")
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
    out(f"alccond: main base = {base:#x}")
    if not base:
        out("no base"); return
    bp_seal.SetEnabled(False)

    at = {}
    for rel, cond in sites:
        bp = target.BreakpointCreateByAddress(base + rel)
        if cond:
            bp.SetCondition(cond)
        bp.SetOneShot(True)
        at[base + rel] = f"main+{rel:#x}" + (f" [{cond}]" if cond else "")
        out(f"bp main+{rel:#x} cond={cond}: id={bp.GetID()}")

    # NOTE: this lldb (batch, Python 3) ignores SetCondition on these one-shot sites — every hit is
    # logged here and the filtering happens in this script, never in lldb. One exception in this loop
    # ends the whole batch silently, so every round is guarded.
    seen = {}
    for round_i in range(48):
        try:
            process.Continue()
            if process.GetState() != lldb.eStateStopped:
                out(f"state {process.GetState()}"); return
            th = process.GetSelectedThread()
            desc = th.GetStopDescription(160)
            f = th.GetFrameAtIndex(0)
            r = {}
            for name in ('x0', 'x1', 'x2', 'x3', 'x8', 'x20', 'x21', 'pc', 'lr'):
                v = f.FindRegister(name)
                r[name] = v.GetValueAsUnsigned() if v else 0
            if 'EXC_BAD_ACCESS' in desc or th.GetStopReason() == lldb.eStopReasonSignal:
                out(f"[SEGV] {desc} pc=main+{r['pc'] - base:#x} lr=main+{r['lr'] - base:#x}")
                return
            rel = r['pc'] - base
            if rel in {s[0] for s in sites}:
                seen[rel] = seen.get(rel, 0) + 1
                out(f"[HIT] main+{rel:#x}#{seen[rel]} lr=main+{r['lr'] - base:#x} " +
                    f"x0={r['x0']:#x} x8={r['x8']:#x} x20={r['x20']:#x} x21={r['x21']:#x}")
            else:
                out(f"[other] main+{rel:#x} ({desc}) lr=main+{r['lr'] - base:#x}")
        except Exception as e:
            out(f"round {round_i} failed: {e!r}"); return
    out("alccond: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f cond_probe.alccond alccond')
