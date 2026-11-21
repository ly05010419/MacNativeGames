"""Provisional memory layouts must leave room for the game's initial heap."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import profile_guess as pg


class DepthSignal(unittest.TestCase):
    ZERO, GL = '[dm] [nvn] depth mode near=0 (as D3D / Metal)\n', '[dm] [nvn] depth mode near=-w (GL)\n'
    RESET = '[dm] [nvn-mtl] depth mode: near=-w (GL), vertex shaders use the main0 entry\n'   # the host's own reset at device init, also printed for a game that never calls the setter

    def profile(self, **keys):
        return type('Profile', (), {'get': staticmethod(lambda k, d='': keys.get(k, d))})

    def seen(self, log, **keys):
        return pg.depth_signal(log, self.profile(**keys)).get('depth_mode', (None,))[0]

    def test_the_game_sets_near_zero(self):
        self.assertEqual(self.seen(self.ZERO), 'zero')

    def test_no_call_is_unknown_not_gl(self):
        self.assertIsNone(self.seen('>>> nnMain\n'))
        self.assertIsNone(self.seen('>>> nnMain\n', depth_mode='zero'))   # a boot that stopped before the call says nothing against the key

    def test_the_host_reset_is_not_the_game_calling(self):
        self.assertIsNone(self.seen(self.RESET, depth_mode='zero'))
        self.assertEqual(self.seen(self.ZERO + self.RESET), 'zero')   # a second device's reset does not undo the game's call

    def test_a_profile_that_agrees_gets_no_change(self):
        self.assertIsNone(self.seen(self.ZERO, depth_mode='zero'))
        self.assertIsNone(self.seen(self.GL))

    def test_the_last_call_counts(self):
        self.assertEqual(self.seen(self.GL + self.ZERO), 'zero')
        self.assertEqual(self.seen(self.ZERO + self.GL, depth_mode='zero'), 'gl')

    def test_the_reason_is_one_comment_line(self):
        value, why = pg.depth_signal(self.ZERO, self.profile())['depth_mode']
        self.assertEqual(value, 'zero'); self.assertIn('near = 0', why); self.assertNotIn('\n', why)


class GlslSources(unittest.TestCase):
    def test_language_preamble_is_not_a_shader(self):
        self.assertEqual(pg.glsl_sources(b'#version 450 core\n#define float4 vec4\n\0void main() {}\0'), 0)

    def test_complete_embedded_sources(self):
        image = b'#version 450\nvoid main() {}\0#version 450\nvoid main (void) {}\0'
        self.assertEqual(pg.glsl_sources(image), 2)

    def test_only_text_files_count_as_glsl_sources(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root / 'shaders').mkdir()
            (root / 'shaders/sprite.vert').write_bytes(b'#version 450\n\tvoid main() {}\r\n')
            (root / 'shaders/sprite.FRAG').write_bytes('// café\nvoid main() {}\n'.encode())
            (root / 'shaders/sprite.txt').write_bytes(b'void main() {}\n')   # not a shader file by its name
            (root / 'shaders/cipher.vert').write_bytes(hashlib.sha256(b'ciphertext').digest() * 128)   # Skyward Sword HD's Dummy.vert: encrypted, read only on a cache miss
            (root / 'shaders/empty.frag').write_bytes(b'')
            self.assertEqual(pg.romfs_glsl_files(root), 2)


class Npdm(unittest.TestCase):
    def npdm(self, flags):
        d = bytearray(0x80); d[:4] = b'META'; d[0xc] = flags
        p = Path(self.tmp.name) / 'main.npdm'; p.write_bytes(d); return p

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)

    def test_64bit_flag(self):
        self.assertTrue(pg.aarch64(self.npdm(0x3f)))   # 64-bit, 39-bit address space

    def test_32bit_program(self):
        self.assertFalse(pg.aarch64(self.npdm(0x04)))   # Pikmin 3 Deluxe: 32-bit, no reserved region

    def test_no_npdm(self):
        self.assertTrue(pg.aarch64(Path(self.tmp.name) / 'missing.npdm'))


class UnrealMemory(unittest.TestCase):
    def test_initial_heap_fits_beside_the_engine_region(self):
        facts = {
            'mods': {'main': {'imports': {'nvnBootstrapLoader', pg.HEAP_API}, 'bss': 0, 'far': False}},
            'romfs': {'root': set(), 'exts': {'.pak': 1}},
            'sysres': 40 * pg.MiB,
            'main_markers': {'Unreal 4'},
        }
        keys = pg.guess(facts, hooks=set())
        reserve, alias, stack = (int(keys[k][0], 0) for k in ('mem_reserve', 'mem_alias', 'mem_stack'))
        self.assertGreaterEqual(alias, 64 * 1024**3)
        # mem_init puts the modules before the alias region, then stacks and the heap.
        heap = reserve - 0x10000000 - alias - stack
        self.assertGreaterEqual(heap, 128 * pg.MiB)



class AddressRegions(unittest.TestCase):
    """region_findings: the host's region table (DM_REGIONS) of a run with a Switch-sized alias region against the profile's alias region"""
    SYMS = {'_ZN2nn3mem17StandardAllocator10InitializeEPvmb': (0x1000, 0x100), '_ZN2nn2os21AllocateAddressRegionEPmm': (0x2000, 0x40)}
    HEAD = '# alias region 0x1000000000; offset\tsize\twho\tmapped\tfirst opened\tend\tvia\n'

    def test_demangle(self):
        self.assertEqual(pg.demangle('_ZN2nn3mem17StandardAllocator10InitializeEPvmb'), 'nn::mem::StandardAllocator::Initialize')
        self.assertEqual(pg.demangle('memcpy'), 'memcpy')

    def test_fits(self):
        t = self.HEAD + '0x4000000\t0x100000000\tmain+0x10\t0x60000000\t0\t0x100000000\t0x1010\n0x104000000\t0x1000000\tmain+0x20\t0\t-\t-\t0x2010\n'
        f = pg.region_findings(t, 0x118000000, self.SYMS)
        self.assertEqual(len(f), 2, f)   # the 16 MB region is under the 256 MB listed, but counts for the reach
        self.assertIn('through nn::mem::StandardAllocator::Initialize', f[0][1])
        self.assertEqual(f[1], (False, "in its first 15 s in nnMain the game's regions reach alias +4.08 GB, within the profile's 4.38 GB alias region (what it takes later in play is not measured here)"))
        self.assertIn('returning to main+0x10', f[0][1])

    def test_standard_allocator_regions_the_host_caps(self):
        # alone: the host's cap makes it fit (not red); with 1.5 GB of regions made after it (Absolum's .NET garbage collector): red, with the stdalloc_cap value that leaves them room
        alone = self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
        f = pg.region_findings(alone, 0x110000000, self.SYMS)
        self.assertFalse(any(red for red, _ in f), f)
        self.assertIn('on the iPhone layout the host gives it 3.94 GB', f[0][1])
        self.assertIn('only by StandardAllocator regions, which the host makes fit', f[-1][1])
        later = alone + '0x304000000\t0x60000000\tmain+0x40\t0x40000000\t0\t0x60000000\t0x2010\n'
        red, text = pg.region_findings(later, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red)
        # the value leaves them room and the host's own 256 MB for what the run did not see: 4.38 - 0.06 - 1.5 - 0.25 GB (Absolum: 2.56 GB, between the 2.5 and 2.72 GB that both play)
        self.assertIn('the regions made after the one returning to main+0x10 take 1.50 GB, more than the 0.25 GB the host leaves them: stdalloc_cap 0xa4000000 (2.56 GB)', text)
        self.assertNotIn('main+0x40 takes', text)   # it fits once the region before it is capped
        self.assertEqual(pg.stdalloc_cap_needed(pg.region_rows(later, self.SYMS), 0x118000000), 0xa4000000)
        self.assertIsNone(pg.stdalloc_cap_needed(pg.region_rows(alone, self.SYMS), 0x110000000))   # the host's own cap does

    def test_the_profiles_stdalloc_cap(self):
        later = (self.HEAD + '0x4000000\t0xa00000000\tmain+0x10\t0x60000000\t0\t0xa00000000\t0x1010\n'
                 '0xa04000000\t0x60000000\tmain+0x40\t0x40000000\t0\t0x60000000\t0x2010\n')
        f = pg.region_findings(later, 0x118000000, self.SYMS, cap=0xa0000000)   # Absolum's 2.5 GB
        self.assertFalse(any(red for red, _ in f), f)
        self.assertIn("the profile's stdalloc_cap gives it 2.50 GB, which leaves the 1.50 GB of regions made after it room", f[0][1])
        red, text = pg.region_findings(later, 0x118000000, self.SYMS, cap=0xc0000000)[-1]
        self.assertTrue(red); self.assertIn("the profile's stdalloc_cap 0xc0000000 leaves the 1.50 GB of regions made after the one returning to main+0x10 too little room: stdalloc_cap 0xa4000000", text)

    def test_no_room_at_all(self):
        t = (self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
             '0x304000000\t0xe0000000\tmain+0x40\t0\t-\t-\t0x2010\n')
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red); self.assertIn('take 3.50 GB, which leaves it no room', text)
        self.assertIsNone(pg.stdalloc_cap_needed(pg.region_rows(t, self.SYMS), 0x118000000))

    def test_a_region_too_big_by_itself(self):
        # the game's own allocator through AllocateAddressRegion (Smash's engine): red, and it does not count as room the StandardAllocator region must leave
        t = (self.HEAD + '0x4000000\t0x300000000\tmain+0x10\t0x60000000\t0\t0x300000000\t0x1010\n'
             '0x304000000\t0xf80000000\tmain+0x30\t0x70000000\t0\t0xf7f210000\t0x2010\n')
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red and text.startswith('in its first 15 s in nnMain'), text)
        self.assertIn('the request returning to main+0x30 takes 62.00 GB through nn::os::AllocateAddressRegion: a smaller one works only if its allocator places nothing by fixed offsets', text)
        self.assertNotIn('stdalloc_cap', text)

    def test_together_too_big(self):
        t = self.HEAD + '0x4000000\t0xc0000000\tmain+0x30\t0\t-\t-\t0x2010\n0xc4000000\t0x80000000\tmain+0x50\t0\t-\t-\t0x2010\n'
        red, text = pg.region_findings(t, 0x118000000, self.SYMS)[-1]
        self.assertTrue(red); self.assertIn('no single region is too big, but together they need more room', text)

    def test_no_table(self):
        self.assertEqual(pg.region_findings('', 0x118000000), [])


if __name__ == '__main__':
    unittest.main()
