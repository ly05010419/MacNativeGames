"""Turn the ??? guest frames of a macOS `sample` report into module+offset and symbols, using the module bases from the
runtime log ('[totk] mapped <name> base=0x...'). usage: sample_guest.py <runtime log> <sample report>"""
import re, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
log, report = (Path(a).read_text(errors='replace') for a in sys.argv[1:3])
bases = [(int(b, 16), int(s), n) for n, b, s in re.findall(r'mapped (\w+) base=(0x[0-9a-f]+) size=(\d+)', log)]
sym = subprocess.run([sys.executable, str(Path(__file__).parent / 'sym.py')], input=''.join(
    f'caller {n}+{a - b:#x}\n' for a in sorted({int(x, 16) for x in re.findall(r'\?\?\?\s+\(in <unknown binary>\)\s+\[(0x[0-9a-f]+)\]', report)})
    for b, s, n in bases if b <= a < b + s), capture_output=True, text=True).stdout
names = dict(line.split(' ', 1) for line in sym.splitlines() if ' ' in line)
def repl(m):
    a = int(m.group(1), 16)
    for b, s, n in bases:
        if b <= a < b + s: k = f'{n}+{a - b:#x}'; return f'{k} {names.get(k, "")[:150]}'
    return m.group(0)
print(re.sub(r'\?\?\?\s+\(in <unknown binary>\)\s+\[(0x[0-9a-f]+)\]', repl, report))
