"""NVN semantics a game can observe (tests/nvn_semantics_probe.c), on the graphics layer's own hle_nvn.c, once per game
configuration that builds from that file. No Metal device, runtime or game data: every symbol hle_nvn.c takes from the
rest of the graphics layer or the runtime is stubbed (returns 0), and only command recording is exercised.

A configuration whose accounting is known to differ from the driver lists the checks it still fails in KNOWN, with the
reason; a known failure that starts passing fails the test too, so the list cannot go stale."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / 'tests/nvn_semantics_probe.c'
TOTK_GFX = ROOT / 'games/totk/third_party/d2r_gfx/gfx'

# The game configurations built from games/totk's graphics layer (games/totk/Makefile: GAME_BOTW for Breath of the Wild,
# GAME_NS2 for Donkey Kong Bananza and Pokemon Pokopia; games/oot/Makefile: GAME_NS2 and GAME_OOT for Ocarina of Time).
CONFIGS = {
    'totk': [],
    'botw': ['-DGAME_BOTW=1'],
    'ns2': ['-DGAME_NS2=1'],
    'oot': ['-DGAME_NS2=1', '-DGAME_OOT=1'],
}
_ACCOUNTING = 'command memory accounting not ported: #1742 fixed it for TOTK only; not measured against the driver here'
KNOWN = {
    'botw': {'CallCommands takes no command memory': _ACCOUNTING, 'CopyCommands takes the copied commands\' size': _ACCOUNTING,
             'bind + call + copy take 20 + 0 + 40': _ACCOUNTING},
    'ns2': {'CallCommands takes no command memory': _ACCOUNTING, 'CopyCommands takes the copied commands\' size': _ACCOUNTING,
            'bind + call + copy take 20 + 0 + 40': _ACCOUNTING},
}
KNOWN['oot'] = KNOWN['ns2']
CFLAGS = ['-std=gnu11', '-O1', '-w', '-fobjc-arc', '-fno-sanitize=all']


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(gfx, defines, work):
    """Compile the probe (which includes hle_nvn.c), stub what the link leaves undefined, link, return the binary."""
    nvn = gfx / 'nvn'
    obj = work / 'probe.o'
    r = run(['xcrun', 'clang', *CFLAGS, *defines, f'-DNVN_HLE_SOURCE="{nvn / "hle_nvn.c"}"', f'-I{nvn}', f'-I{nvn / "include"}',
             '-c', str(PROBE), '-o', str(obj)])
    if r.returncode: raise AssertionError('compile failed:\n' + r.stderr[-4000:])
    objs = [str(obj)]
    for asm in ('nvn_thunks.S', 'nvn_entry.S'):   # the NVN name table and slots (generated from names.txt) and the entry stubs
        if (nvn / asm).exists():
            o = work / (asm + '.o'); objs.append(str(o))
            r = run(['xcrun', 'clang', '-c', str(nvn / asm), '-o', str(o)])
            if r.returncode: raise AssertionError(f'{asm} failed:\n' + r.stderr[-4000:])
    exe = work / 'probe'
    link = ['xcrun', 'clang', *objs, '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'QuartzCore',
            '-framework', 'CoreGraphics', '-framework', 'IOKit', '-framework', 'CoreFoundation', '-o', str(exe)]
    r = run(link)
    if r.returncode:
        missing = sorted(set(re.findall(r'^\s+"_([A-Za-z0-9_$]+)", referenced from', r.stderr, re.M)))
        if not missing: raise AssertionError('link failed:\n' + r.stderr[-4000:])
        stubs = work / 'stubs.c'
        stubs.write_text(''.join(f'long {s}(void) {{ return 0; }}\n' for s in missing))
        r2 = run(['xcrun', 'clang', '-w', '-c', str(stubs), '-o', str(work / 'stubs.o')])
        if r2.returncode: raise AssertionError('stubs failed:\n' + r2.stderr[-4000:])
        r = run(link[:2 + len(objs)] + [str(work / 'stubs.o')] + link[2 + len(objs):])
        if r.returncode: raise AssertionError('link with stubs failed:\n' + r.stderr[-4000:])
    return exe


def results(exe):
    r = run([str(exe)], timeout=60)
    lines = r.stdout.splitlines()
    if not lines or not lines[-1].startswith('done '):
        raise AssertionError(f'probe did not finish (exit {r.returncode}):\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    out = {}
    for line in lines[:-1]:
        m = re.match(r'(ok|FAIL) ([^:]+?)(?:: (.*))?$', line)
        if m: out[m.group(2)] = (m.group(1) == 'ok', m.group(3) or '')
    return out


class NvnSemantics(unittest.TestCase):
    def check(self, name, gfx=TOTK_GFX):
        with tempfile.TemporaryDirectory() as d:
            got = results(build(gfx, CONFIGS[name], Path(d)))
        self.assertTrue(got, 'no checks ran')
        known = KNOWN.get(name, {})
        for check, (ok, detail) in got.items():
            with self.subTest(config=name, check=check):
                if check in known:
                    self.assertFalse(ok, f'now passes; remove it from KNOWN[{name!r}] ({known[check]})')
                else:
                    self.assertTrue(ok, detail)
        self.assertFalse(set(known) - set(got), 'KNOWN lists checks the probe no longer has')

    def test_totk(self): self.check('totk')
    def test_botw(self): self.check('botw')
    def test_ns2(self): self.check('ns2')
    def test_oot(self): self.check('oot')


if __name__ == '__main__':
    unittest.main()
