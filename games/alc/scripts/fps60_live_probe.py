"""lldb probe: attach to the running game and read the frame-step timer object live.

With the 60 FPS patch the step (timer+0xd0, in 30 fps frame units) must read ~0.5 with the measured
frame time (+0xe0) ~16.7 ms; +0xd8 publishes whole ticks (alternating 0/1) and +0xd4 the fractional
carry. A step of ~1.0 here would mean the game runs 2x fast.

Run (from AC_build, with the game running):
  lldb -b -o "command script import ../scripts/fps60_live_probe.py" -o alcfps60 -o quit -- <pid>
"""
import lldb
import struct
import sys
import time


def out(msg):
    print(msg)
    sys.stdout.flush()


def alcfps60(debugger, command, exe_ctx, result, _dict):
    pid = int(command.strip())
    err = lldb.SBError()
    listener = debugger.GetListener()
    target = debugger.CreateTarget('')
    process = target.AttachToProcessWithID(listener, pid, err)
    if not process or not process.IsValid():
        out(f"attach failed: {err}"); return
    frame = process.GetSelectedThread().GetFrameAtIndex(0)
    base = int(frame.EvaluateExpression('(unsigned long)modules[0].base').GetValueAsUnsigned())
    out(f"main base {base:#x}")
    obj = struct.unpack('<Q', process.ReadMemory(base + 0xdb89a0, 8, err))[0]
    if not obj:
        out("timer object pointer is null"); return
    out(f"timer object {obj:#x} (main+{obj - base:#x})")
    for i in range(8):
        data = process.ReadMemory(obj + 0xd0, 0x10, err)
        if not data:
            out(f"read failed: {err}"); return
        step, frac, whole, frame_ms = struct.unpack('<ffff', data)
        out(f"sample {i}: step {step:.3f}  frac {frac:.3f}  whole {whole}  frame_ms {frame_ms:.2f}")
        time.sleep(0.4)
    process.Detach()
    out("alcfps60: done")


def __lldb_init_module(debugger, _dict):
    debugger.HandleCommand('command script add -f fps60_live_probe.alcfps60 alcfps60')
