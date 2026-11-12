"""Test dispatch with private rewrite records and fake handlers; never execute guest code."""
import argparse
import ctypes
import subprocess
import sys
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exefs', type=Path, required=True)
parser.add_argument('--rewritten', type=Path, required=True)
args = parser.parse_args()
game = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(game / 'scripts'))
from rewrite_modules import MODULES
from module_metadata import verify_rewrite_report
verify_rewrite_report(args.exefs.resolve(), args.rewritten.resolve())
repo = game / 'runtime'

class Gate(ctypes.Structure):
    _fields_ = [('address', ctypes.c_size_t), ('kind', ctypes.c_uint32), ('arg', ctypes.c_uint32)]

entries = []
for i, name in enumerate(MODULES):
    for line in (args.rewritten / (name + '.gates.tsv')).read_text().splitlines():
        address, kind, arg, _ = line.split()
        entries.append(Gate(0x100000000 + i * 0x10000000 + int(address, 16),
                            {'svc': 1, 'cntpct': 2, 'cntvct': 2}[kind], int(arg, 0)))

with tempfile.TemporaryDirectory() as tmp:
    lib = Path(tmp) / 'gates.dylib'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                    str(repo / 'gates.c'), '-o', str(lib)], check=True)
    api = ctypes.CDLL(str(lib))
    svc_t = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64))
    timer_t = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64))
    def svc(_, number, registers):
        registers[0] = number + 1000
        return 1
    def timer(_, ticks):
        ticks[0] = 12345
        return 1
    svc_cb, timer_cb = svc_t(svc), timer_t(timer)
    api.tmnt_gates_create.argtypes = [ctypes.POINTER(Gate), ctypes.c_size_t, ctypes.POINTER(ctypes.c_void_p)]
    api.tmnt_gate_dispatch.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint64), svc_t, timer_t, ctypes.c_void_p]
    api.tmnt_gates_destroy.argtypes = [ctypes.c_void_p]
    index = ctypes.c_void_p()
    array = (Gate * len(entries))(*entries)
    assert api.tmnt_gates_create(array, len(entries), ctypes.byref(index)) == 0
    for entry in entries:
        registers = (ctypes.c_uint64 * 31)()
        assert api.tmnt_gate_dispatch(index, entry.address, registers, svc_cb, timer_cb, None) == 0
        assert registers[0 if entry.kind == 1 else entry.arg] == (entry.arg + 1000 if entry.kind == 1 else 12345)
    api.tmnt_gates_destroy(index)
    print('validated gate records:', len(entries), '; SVC:', sum(x.kind == 1 for x in entries),
          '; counter:', sum(x.kind == 2 for x in entries), '; game code executed: no')
