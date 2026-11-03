"""lldb probe: attach to the parked game, read the +0x194 spin flag, watch it for writes.

Run (from AC_build, with the game parked): lldb -b -o "command script import ../scripts/live_flag_probe.py" -o alclive -o quit -- <pid>
"""
import lldb
import struct
import sys
import time

FLAG_OFFSET = 0x194


def out(msg):
    print(msg)
    sys.stdout.flush()


def alclive(debugger, command, exe_ctx, result, _dict):
    pid = int(command.strip())
    err = lldb.SBError()
    listener = debugger.GetListener()
    target = debugger.CreateTarget('')
    process = target.AttachToProcessWithID(listener, pid, err)
    if not process or not process.IsValid():
        out(f"attach failed: {err}"); return
    base_v = target.EvaluateExpression('(unsigned long)modules[0].base')
    base = int(base_v.GetValueAsUnsigned())
    obj_v = target.EvaluateExpression(f'*(unsigned long *)(modules[0].base + 0xdb84c8)')
    obj = int(obj_v.GetValueAsUnsigned())
    out(f"base {base:#x} object {obj:#x} (main+{obj - base:#x})")
    flag = obj + FLAG_OFFSET
    data = process.ReadMemory(flag, 4, err)
    out(f"flag @main+{FLAG_OFFSET:#x} = {struct.unpack('<I', data)[0]:#x}" if data else f"read failed: {err}")
    # neighbours for context
    data = process.ReadMemory(obj + 0x180, 0x30, err)
    if data:
        vals = struct.unpack('<12I', data)
        out("fields +0x180..: " + ' '.join(f"{v:#x}" for v in vals))
    wp = target.WatchAddress(flag, 4, False, True, err)
    out(f"watchpoint valid={wp.IsValid()} err={err}")
    if not wp.IsValid():
        process.Detach(); return
    # poll for a write for up to 15 s, then report and leave the process as it was
    deadline = time.time() + 15
    saw_write = False
    while time.time() < deadline:
        state = process.GetState()
        if state == lldb.eStateStopped:
            th = process.GetSelectedThread()
            if th.GetStopReason() == lldb.eStopReasonWatchpoint:
                f = th.GetFrameAtIndex(0)
                pc = f.FindRegister('pc').GetValueAsUnsigned()
                lr = f.FindRegister('lr').GetValueAsUnsigned()
                out(f"WRITE seen from pc=main+{pc - base:#x} lr=main+{lr - base:#x} " +
                    f"stop={th.GetStopDescription(120)}")
                saw_write = True
            break
        time.sleep(0.2)
    if not saw_write:
        out("no write to the flag in 15 s: the parked spin waits on nobody")
    wp.SetEnabled(False)
    process.Detach()
    out("alclive: done")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f live_flag_probe.alclive alclive')
