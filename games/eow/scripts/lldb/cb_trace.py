import lldb, lldbcb
def hit(frame, bp_loc, d):
    a = frame.GetPC() - lldbcb.B['main']
    lldbcb.log('main+%#x tid %d x0=%#x x8=%#x x19=%#x x20=%#x x21=%#x x27=%#x' % (a, frame.GetThread().GetThreadID(), *(lldbcb.reg(frame, r) for r in ('x0','x8','x19','x20','x21','x27'))))
    return False
def arm(debugger, command, result, internal_dict):
    t = debugger.GetSelectedTarget()
    for s in command.split():
        bp = t.BreakpointCreateByAddress(lldbcb.B['main'] + int(s, 16))
        bp.SetScriptCallbackFunction('cb_trace.hit')
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f cb_trace.arm arm')
