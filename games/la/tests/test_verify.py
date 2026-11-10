"""The package checks: shader libraries with the two excepted shaders, their origin, and the physics cut; synthetic data only."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME/'scripts'))
import shader_origin  # noqa: E402
import verify_la as verify  # noqa: E402

KEY_OK, KEY_X = '0123456789abcdef', 'fedcba9876543210'


def bnsh(stages):
    """A BNSH with one variation whose program has the given {slot: (control, code)}, and the keys they make."""
    d = bytearray(0x400); d[:8] = b'BNSH\0\0\0\0'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)
    struct.pack_into('<Q', d, 0x110, 0x140)
    keys = {}
    for slot, (ctl, code) in stages.items():
        at = len(d); d += bytes(0x20); struct.pack_into('<Q', d, 0x148+8*slot, at)
        c_at = len(d); d += ctl; k_at = len(d); d += code
        struct.pack_into('<QQII', d, at+8, c_at, k_at, len(code), len(ctl))
        keys[hashlib.sha256(ctl[:shader_origin.control_length(ctl)]+code).hexdigest()[:16]] = shader_origin.STAGES[slot]
    return bytes(d), keys


class OriginTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.romfs = Path(self.tmp.name)
        ctl = bytearray(0x880); ctl[:4] = b'CTL!'
        self.data, keys = bnsh({0: (bytes(ctl), b'vertex'), 4: (bytes(ctl), b'pixel')})
        self.key = next(k for k, s in keys.items() if s == 'ps')
        self.vkey = next(k for k, s in keys.items() if s == 'vs')
        (self.romfs/'post.bfsha').write_bytes(bytes(0x40)+self.data)
        self.entry = dict(key=self.key, stage='pixel', file='post.bfsha', bnsh='0x40', variation=0)

    def test_origin_and_stage_from_the_file(self):
        shader_origin.check(self.romfs, self.entry)

    def test_rejects_other_stage_offset_variation_and_file(self):
        for change, why in ((dict(key=self.vkey), 'stage differs'), (dict(bnsh='0x0'), 'no BNSH'), (dict(variation=1), 'variations'),
                            (dict(file='other.bfsha'), 'origin missing'), (dict(key='0'*16), 'origin differs')):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, why):
                shader_origin.check(self.romfs, dict(self.entry, **change))

    def test_rejects_a_bnsh_without_grsc(self):
        (self.romfs/'post.bfsha').write_bytes(bytes(0x40)+self.data[:0x60]+b'xxxx'+self.data[0x64:])
        with self.assertRaisesRegex(ValueError, 'no grsc'):
            shader_origin.check(self.romfs, self.entry)


class ExceptionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name); self.libs = root/'b-test'; self.libs.mkdir(); self.romfs = root/'romfs'; self.romfs.mkdir()
        ctl = bytearray(0x880); ctl[:4] = b'CTL!'
        data, keys = bnsh({4: (bytes(ctl), b'pixel')})
        self.kx = next(iter(keys))
        (self.romfs/'post.bfsha').write_bytes(data)
        self.map = root/'map.tsv'
        self.map.write_text(f'package\tindex\tstage\tdm_ctl_sha16\nvs_a.bin\t0\tvertex\t{KEY_OK}\nps_shader.bin\t1\tpixel\t{self.kx}\n')
        self.exc = [dict(key=self.kx, package='ps_shader.bin', stage='pixel', file='post.bfsha', bnsh='0x0', variation=0, st='MSL failed')]
        (self.libs/f'{KEY_OK}_b.metallib').write_bytes(b'MTLBsynthetic')
        (self.libs/f'{KEY_OK}_b.json').write_text(json.dumps({'chain': 'test', 'dm_ctl': KEY_OK, 'stage': 'vs'}))
        self.outcome({KEY_OK: 'ok', self.kx: 'MSL failed'})

    def outcome(self, states):
        (self.libs/'_b_build.json').write_text(json.dumps([{'key': k, 'st': s, 'err': 'synthetic'} for k, s in states.items()]))

    def check(self):
        return verify.verify_libraries(self.libs, self.map, self.exc, self.romfs)

    def test_absent_exception_passes(self):
        self.assertEqual(self.check()['exceptions'], {self.kx: 'absent'})

    def test_other_failed_key_is_rejected(self):
        self.outcome({KEY_OK: 'MSL failed', self.kx: 'MSL failed'})
        with self.assertRaisesRegex(ValueError, 'outside the exception'): self.check()

    def test_another_missing_library_is_still_rejected(self):
        (self.libs/f'{KEY_OK}_b.metallib').unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete offline shader key'): self.check()

    def test_half_built_exception_is_rejected(self):
        (self.libs/f'{self.kx}_b.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'half built'): self.check()

    def test_exception_failing_another_way_is_rejected(self):
        self.outcome({KEY_OK: 'ok', self.kx: 'translation failed'})
        with self.assertRaisesRegex(ValueError, 'failed another way'): self.check()

    def test_exception_row_must_be_pixel_in_its_package(self):
        self.map.write_text(self.map.read_text().replace('ps_shader.bin\t1\tpixel', 'cs_shader.bin\t1\tcompute'))
        with self.assertRaisesRegex(ValueError, 'row differs'): self.check()

    def test_exception_outside_the_map_is_rejected(self):
        self.outcome({KEY_OK: 'ok', self.kx: 'ok'})
        self.exc[0]['key'] = '1'*16
        with self.assertRaisesRegex(ValueError, 'not in the map'): self.check()

    def test_built_exception_follows_the_normal_rules(self):
        self.outcome({KEY_OK: 'ok', self.kx: 'ok'})
        (self.libs/f'{self.kx}_b.json').write_text(json.dumps({'chain': 'test', 'dm_ctl': self.kx, 'stage': 'ps'}))
        with self.assertRaisesRegex(ValueError, 'incomplete offline shader key'): self.check()
        (self.libs/f'{self.kx}_b.metallib').write_bytes(b'MTLBsynthetic')
        self.assertEqual(self.check()['exceptions'], {self.kx: 'present'})

    def test_the_target_lists_exactly_the_two_shaders(self):
        shaders = json.loads((GAME/'assets/shader-exceptions.json').read_text())['shaders']
        self.assertEqual({s['key'] for s in shaders}, {'9d8f6b8543355374', '9338e3f1b597fe7c'})
        self.assertEqual({(s['stage'], s['package'], s['file'], s['bnsh']) for s in shaders}, {('pixel', 'ps_shader.bin', 'region_common/shader/postprocess.bfsha', '0x252000')})


class CutTests(unittest.TestCase):
    def nso(self, word):
        import lz4.block
        text = bytearray(verify.CUT['offset']+0x10); text[verify.CUT['offset']:verify.CUT['offset']+4] = bytes.fromhex(word)
        packed = lz4.block.compress(bytes(text), store_size=False)
        b = bytearray(256); b[:4] = b'NSO0'; b[0x40:0x50] = bytes.fromhex(verify.CUT['build'])
        struct.pack_into('<I', b, 12, 1); struct.pack_into('<III', b, 0x10, 256, 0, len(text)); struct.pack_into('<I', b, 0x60, len(packed))
        return bytes(b)+packed, bytes(text)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def write(self, original_word, patched_word, rows):
        nso, text = self.nso(original_word)
        (self.root/'main').write_bytes(nso)
        out = bytearray(text); out[verify.CUT['offset']:verify.CUT['offset']+4] = bytes.fromhex(patched_word)
        (self.root/'main.patched.bin').write_bytes(bytes(out))
        (self.root/'main.cuts.tsv').write_text('# BUILD_ID\tx\n'+''.join('\t'.join(r)+'\treason\n' for r in rows))

    def test_cut_checked_from_outside_the_rewriter(self):
        row = [f'{verify.CUT["offset"]:#x}', verify.CUT['original'], verify.CUT['new']]
        self.write(verify.CUT['original'], verify.CUT['new'], [row])
        verify.verify_cut(self.root/'main', self.root)
        for original, patched, rows, why in ((verify.CUT['original'], verify.CUT['original'], [row], 'output differs'),
                                             ('00000000', verify.CUT['new'], [row], 'original differs'),
                                             (verify.CUT['original'], verify.CUT['new'], [], 'unexpected cuts'),
                                             (verify.CUT['original'], verify.CUT['new'], [row, row], 'unexpected cuts')):
            with self.subTest(why=why):
                self.write(original, patched, rows)
                with self.assertRaisesRegex(ValueError, why): verify.verify_cut(self.root/'main', self.root)


if __name__ == '__main__':
    unittest.main()
