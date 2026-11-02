import lldb
OUT = open('logs/lldbcb.out', 'w')
B = {}
def setup(debugger, command, result, internal_dict):
    f = debugger.GetSelectedTarget().GetProcess().GetSelectedThread().GetSelectedFrame()
    for m in ('main', 'sdk'):
        B[m] = int(f.EvaluateExpression('(unsigned long)runtime_module_base("%s")' % m).GetValue())
    OUT.write('bases %s\n' % {k: hex(v) for k, v in B.items()}); OUT.flush()
def off(a):
    if B['main'] <= a < B['main'] + 0x3520000: return 'main+%#x' % (a - B['main'])
    if B['sdk'] <= a < B['sdk'] + 0xdc0000: return 'sdk+%#x' % (a - B['sdk'])
    return hex(a)
def walk(frame, n=10):
    p = frame.GetThread().GetProcess(); e = lldb.SBError()
    out = [off(frame.GetPC()), off(frame.FindRegister('lr').GetValueAsUnsigned())]
    fp = frame.FindRegister('fp').GetValueAsUnsigned()
    for _ in range(n):
        if not fp or fp & 15: break
        lr = p.ReadPointerFromMemory(fp + 8, e); nfp = p.ReadPointerFromMemory(fp, e)
        out.append(off(lr))
        if nfp == fp: break
        fp = nfp
    return ' <- '.join(out)
def reg(frame, r): return frame.FindRegister(r).GetValueAsUnsigned()
def log(s): OUT.write(s + '\n'); OUT.flush()
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f lldbcb.setup cbsetup')
def allthreads(debugger, command, result, internal_dict):
    p = debugger.GetSelectedTarget().GetProcess()
    for t in p:
        f = t.GetFrameAtIndex(0)
        log('T%d %s | %s' % (t.GetIndexID(), t.GetName() or '', walk(f, 12)))
        # host frames too
        log('   host: ' + ' <- '.join((fr.GetFunctionName() or hex(fr.GetPC())) for fr in list(t)[:8]))
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f lldbcb.setup cbsetup')
    debugger.HandleCommand('command script add -f lldbcb.allthreads allthreads')
