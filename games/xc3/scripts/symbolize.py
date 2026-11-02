#!/usr/bin/env python3
"""Name the functions in a runtime call chain: reads "caller <module>+0x<offset>" lines (or arguments like sdk+0x1234)
and prints the defined symbol containing each address, from <work>/analysis/runtime-layout.json (prepare_runtime.py).
Symbols are the modules' own exports; main exports little, so main addresses often stay unnamed.
The work folder is XC3_WORK when set, else XC3_build next to this folder's Makefile."""
import bisect, json, os, re, subprocess, sys
from pathlib import Path

work = Path(os.environ.get('XC3_WORK') or Path(__file__).resolve().parents[1] / 'XC3_build')
modules = {m['name']: m for m in json.loads((work / 'analysis/runtime-layout.json').read_text())}
tables = {}
for name, m in modules.items():
    rows = sorted((s['value'], s['size'], s['name']) for s in m['symbols'] if s['defined'] and s['type'] == 2 and s['value'])
    tables[name] = ([r[0] for r in rows], rows)

def demangle(names):
    try: return subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    except OSError: return names

text = ' '.join(sys.argv[1:]) if len(sys.argv) > 1 else sys.stdin.read()
hits = re.findall(r'\b(main|sdk|subsdk[0-9]+)\+0x([0-9a-f]+)', text)
out = []
for mod, off in hits:
    off = int(off, 16); starts, rows = tables[mod]
    i = bisect.bisect_right(starts, off) - 1
    if i >= 0 and (rows[i][1] == 0 or off < rows[i][0] + rows[i][1]): out.append((mod, off, rows[i][2], off - rows[i][0]))
    else: out.append((mod, off, '?', 0))
names = demangle([o[2] for o in out])
for (mod, off, _, delta), n in zip(out, names):
    print(f'{mod}+{off:#x}  {n}' + (f' +{delta:#x}' if n != '?' else ''))
