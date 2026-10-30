"""Symbolize '[totk] caller <module>+<offset>' lines from a runtime log (stdin) with the loader's symbol tables."""
import bisect, json, subprocess, sys
from pathlib import Path
mods = {m['name']: m for m in json.loads((Path(__file__).resolve().parents[2] / 'analysis/runtime-layout.json').read_text())}
tables = {}
def name(mod, off):
    if mod not in tables:
        tables[mod] = sorted((s['value'], s['size'], s['name']) for s in mods[mod]['symbols'] if s['defined'] and s['type'] == 2)
    fs = tables[mod]; i = bisect.bisect_right([f[0] for f in fs], off) - 1
    if i < 0: return '?'
    v, size, n = fs[i]
    n = subprocess.run(['c++filt', n], capture_output=True, text=True).stdout.strip() or n
    return f'{n}+{off - v:#x}' + ('' if off - v < size or not size else ' (beyond size)')
for line in sys.stdin:
    t = line.split('caller ')[-1].strip()
    if 'caller ' in line and '+' in t:
        mod, off = t.split('+'); print(t, name(mod, int(off, 16)))
