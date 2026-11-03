"""Link generated timing instructions into a native Apple-silicon test binary.

ADRP data addresses are relocated by the static linker; exit branches become
RET. Arithmetic and native counter reads execute directly on the CPU. No game
files are loaded. These clock checks do not establish cutscene correctness.
"""
import importlib.util
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'runtime/tools'))
spec = importlib.util.spec_from_file_location('aoc_patches_test', GAME / 'runtime/games/01002B00111A2000/patches.py')
patches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patches)


def native_assembly():
    pc = 0x3100000
    words, sites, originals = {}, {}, {}
    def emit(word):
        nonlocal pc
        words[pc] = word
        pc += 4
    def put(site, encoded, reason):
        assert len(encoded) == 1 and encoded[0] >> 26 == 5
        imm = encoded[0] & 0x3ffffff
        sites[site] = site + ((imm ^ 0x2000000) - 0x2000000) * 4
    ctx = SimpleNamespace(here=lambda: pc, emit=emit, state=0x3000000,
                          site=lambda site, original: originals.update({site: original}), put=put)
    patches.frame_step(ctx)
    assert ctx.aoc_timing_abi == 3
    assert originals == {0x166dab4: '68734339', 0x166dc60: 'e83640b9681a00b9e04640bd',
                         0x8d94b0: '4900c0f2', 0x8c3610: '295d41bd'}
    names = {0x8d94b0: 'device', 0x166dab4: 'task', 0x166dc60: 'step', 0x8c3610: 'scene'}
    exits = {site + (12 if site == 0x166dc60 else 4) for site in sites}
    entries = {address: names[site] for site, address in sites.items()}
    pages, output = set(), ['.text', '.p2align 2']
    for address, word in sorted(words.items()):
        if address in entries:
            name = '_timing_' + entries[address]
            output += [f'.globl {name}', name + ':']
        if word & 0x9f000000 == 0x90000000:
            imm = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
            page = (address & ~4095) + ((imm ^ 0x100000) - 0x100000) * 4096
            assert 0 <= page < 0x4000000
            pages.add(page)
            output.append(f'adrp x{word & 31}, _timing_page_{page:x}@PAGE')
        elif word >> 26 == 5:
            imm = word & 0x3ffffff
            target = address + ((imm ^ 0x2000000) - 0x2000000) * 4
            if target in exits:
                output.append('ret')
            else:
                assert target in words
                output.append(f'.long 0x{word:08x}')
        else:
            output.append(f'.long 0x{word:08x}')
    # Explicit zero-fill page labels produce ordinary Mach-O relocations. The
    # image is writable data, never executable, and occupies no on-disk payload.
    output += ['.section __DATA,__aoc_test,zerofill', '.p2align 14',
               '.globl _timing_image', '_timing_image:']
    previous = 0
    for page in sorted(pages):
        output += [f'.space {page - previous}', f'_timing_page_{page:x}:']
        previous = page
    output.append(f'.space {0x4000000 - previous}')
    return '\n'.join(output) + '\n'


@unittest.skipUnless(sys.platform == 'darwin' and platform.machine() == 'arm64',
                     'native timing probe requires Apple silicon macOS')
class DynamicTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        root = Path(cls.tmp.name)
        assembly = root / 'timing_stubs.S'
        assembly.write_text(native_assembly())
        cls.binary = root / 'native-timing-probe'
        subprocess.run(['clang', '-arch', 'arm64', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(assembly), str(GAME / 'tests/native_timing_call.S'),
                        str(GAME / 'tests/native_timing_probe.c'), '-o', str(cls.binary)], check=True)
    def probe(self, name):
        result = subprocess.run([str(self.binary), name], text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    def test_supported_target_clocks(self): self.probe('targets')
    def test_variable_intervals(self): self.probe('variable')
    def test_live_switches(self): self.probe('switches')
    def test_fixed_step_and_pause_bounds(self): self.probe('bounds')
    def test_original_timescale(self): self.probe('timescale')
    def test_scene_cadence_is_independent(self): self.probe('scene')
    def test_scene_original_and_long_gap(self): self.probe('scene-bounds')
    def test_device_interval_and_absent_device(self): self.probe('device')


if __name__ == '__main__': unittest.main()
