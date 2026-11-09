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


def recorded(layout):
    """What frame_step asks of the rewriter for one build: originals asserted, words emitted, branches placed."""
    pc = 0x3100000
    originals, words, placed = {}, [], {}
    def emit(word):
        nonlocal pc
        words.append(word)
        pc += 4
    ctx = SimpleNamespace(here=lambda: pc, emit=emit, state=0x3000000,
                          site=lambda site, original: originals.update({site: original}),
                          put=lambda site, encoded, reason: placed.update({site: encoded[0]}))
    patches.frame_step(ctx, at=layout)
    return originals, words, placed


def store_word(reg, base, offset):
    return 0xb9000000 | offset // 4 << 10 | base << 5 | reg


class BuildLayouts(unittest.TestCase):
    def test_every_build_has_cuts_and_stubs(self):
        self.assertEqual(set(patches.CUTS), set(patches.FRAME_STEP))
        self.assertEqual(len(patches.FRAME_STEP), 2)

    def test_1_4_1_asserts_its_own_sites(self):
        originals, _, placed = recorded(patches.LAYOUT_141)
        expected = {0x1670124: '68734339', 0x16702d0: 'e83640b9681a00b9e04640bd',
                    0x8d9ae0: '4900c0f2', 0x8c3c40: '298d42bd',
                    0x18f8f30: '20a9fb97', 0x18f969c: 'a10303d1'}   # output 1440p: the UI 2D node's scale
        expected.update(patches.ASPECT_STORES)   # aspect ratio: the six camera aspect stores
        self.assertEqual(originals, expected)
        self.assertEqual(set(placed), set(originals))

    def test_1_4_1_aspect_stubs_scale_only_wide_views(self):
        _, words, _ = recorded(patches.LAYOUT_141)
        for site, original in patches.ASPECT_STORES.items():
            store = int.from_bytes(bytes.fromhex(original), 'little'); src = store & 31
            slot = 0x8c if site == 0x115fa64 else 0x88
            self.assertIn(0xbd400000 | slot // 4 << 10 | 16 << 5 | 16, words)   # ldr s16,[x16,#slot]: the factor
            self.assertIn(0x1e312000 | src << 5, words)                          # fcmp s<src>,s17 (1.5): views narrower than 3:2 keep theirs
            self.assertIn(0x1e300811 | src << 5, words)                          # fmul s17,s<src>,s16
            self.assertIn(store, words)                                          # the original store when the factor is 0
            self.assertIn((store & ~31) | 17, words)                             # the scaled aspect stored in its place
        self.assertIn(0x1e2f1011, words)                                         # fmov s17,#1.5

    def test_1_4_1_writes_its_own_frame_rate_block(self):
        _, words, _ = recorded(patches.LAYOUT_141)
        for offset in (0x280, 0x284, 0x288):
            self.assertIn(store_word(8, 17, offset), words)
        self.assertNotIn(store_word(8, 17, 0x150), words)
        self.assertIn(0xbd428d29, words)   # the scene's original path keeps this build's load

    def test_1_3_0_is_the_default_layout(self):
        self.assertEqual(recorded(patches.LAYOUT_130)[0][0x8c3610], '295d41bd')


if __name__ == '__main__': unittest.main()
