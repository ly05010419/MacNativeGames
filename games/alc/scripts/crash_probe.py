"""lldb-driven crash inspection for the Astral Chain stage-3 boot.

Run (from AC_build):
  lldb -b build/alc-native -o "command script import ../scripts/crash_probe.py" -o alcprobe -o quit

Waits for main's seal, reads the module base dynamically (it is not stable across runs), reads the
registry object pointer at base+0xdc2690 (the crash walk loads it: x23 = [0xdc2690], then reads
x23->field_28 and dereferences it — the earlier 0xcc9cd98 addend reading was the wrong object), arms
write watchpoints on the object's first field and on field +0x28, breaks once at the crash function's
entry (base+0x9c0cd0), then continues: a watch hit names the writer (pc/lr are printed as main+offsets),
EXC_BAD_ACCESS ends the run with the guest state dumped.
"""
import lldb
import os
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcprobe(debugger, command, exe_ctx, result, _dict):
    target = debugger.GetSelectedTarget()
    out("alcprobe: arming")
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
    base_v = frame.EvaluateExpression('(uint8_t*)modules[0].base')
    base = int(base_v.GetValueAsUnsigned() or 0)
    out(f"alcprobe: main base = {base:#x}")
    if not base:
        out("no base"); return

    data = process.ReadMemory(base + 0xdc2690, 8, err)
    if not data:
        out(f"object pointer read failed: {err}"); return
    obj = struct.unpack('<Q', data)[0]
    if not obj:
        out("object pointer is null at seal"); return
    out(f"registry object = {obj:#x} (main+{obj - base:#x})")
    field = obj + 0x28

    try:
        bp_seal.SetEnabled(False)
        wp_field = target.WatchAddress(field, 8, False, True, err)   # read=off, write=on
        out(f"watchpoint on {field:#x} (field_28): valid={wp_field.IsValid()} err={err}")
        wp_head = target.WatchAddress(obj, 8, False, True, err)
        out(f"watchpoint on {obj:#x} (field_00): valid={wp_head.IsValid()} err={err}")
        if not wp_field.IsValid() or not wp_head.IsValid():
            out("watch failed"); return
    except Exception as e:
        out(f"arm failed: {e!r}"); return
    try:
        bp_crash = target.BreakpointCreateByAddress(base + 0x9c0cd0)
        bp_crash.SetOneShot(True)
        out(f"entry bp: id={bp_crash.GetID()}")
    except Exception as e:
        out(f"entry bp failed (continuing on the watches alone): {e!r}")

    def dump(tag):
        try:
            _dump(tag)
        except Exception as e:
            out(f"dump failed: {e!r}")

    def _dump(tag):
        th = process.GetSelectedThread()
        f = th.GetFrameAtIndex(0)
        regs = {}
        for r in ('x0', 'x1', 'x2', 'x3', 'x8', 'x9', 'x10', 'x21', 'x23', 'x24', 'pc', 'lr'):
            v = f.FindRegister(r)
            regs[r] = v.GetValueAsUnsigned() if v else 0
        out(f"[{tag}] stop={th.GetStopDescription(120)}")
        out(f"[{tag}] " + ' '.join(f"{k}={v:#x}" for k, v in regs.items()))
        for k in ('pc', 'lr', 'x8', 'x9', 'x10', 'x21', 'x23'):
            if base <= regs[k] < base + (1 << 28):
                out(f"[{tag}] {k} = main+{regs[k] - base:#x}")
        for name, addr, n in (('object', obj, 16), ('ptrtable', base + 0xdc2688, 6)):
            data = process.ReadMemory(addr, 8 * n, err)
            if data:
                vals = struct.unpack(f'<{n}Q', data)
                pieces = []
                for v in vals:
                    pieces.append(f"main+{v - base:#x}" if base <= v < base + (1 << 28) else f"{v:#x}")
                out(f"[{tag}] {name} @main+{addr - base:#x}: " + ' '.join(pieces))
            else:
                out(f"[{tag}] {name} read failed: {err}")

    for round_i in range(12):
        process.Continue()
        if process.GetState() != lldb.eStateStopped:
            out(f"state {process.GetState()}"); return
        th = process.GetSelectedThread()
        reason = th.GetStopReason()
        desc = th.GetStopDescription(160)
        if 'EXC_BAD_ACCESS' in desc or reason == lldb.eStopReasonSignal:
            dump("SEGV"); return
        if reason == lldb.eStopReasonWatchpoint:
            dump(f"watch#{round_i}")
        elif reason == lldb.eStopReasonBreakpoint:
            dump(f"entry#{round_i}")
        else:
            dump(f"other#{round_i}({reason})")
    out("alcprobe: rounds exhausted")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f crash_probe.alcprobe alcprobe')
