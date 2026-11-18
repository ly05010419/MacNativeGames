"""What extract_ss.py reads from the extractor's log, and the tolerance mechanism for a named base meta (a synthetic exception: this target names none); synthetic logs only."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME/'scripts'))
import extract_ss  # noqa: E402

REAL = json.loads((GAME/'assets/integrity-exceptions.json').read_text())['base_meta']   # None: every Skyward Sword HD container checked so far is signed
EXC = dict(container_sha256='a'*64, meta_nca='0'*32+'.cnmt.nca', meta_sha256='b'*64, program_sha256='c'*64)   # a synthetic exception for the mechanism
_tmp = tempfile.TemporaryDirectory(); (Path(_tmp.name)/'assets').mkdir()
(Path(_tmp.name)/'assets/integrity-exceptions.json').write_text(json.dumps(dict(reason='synthetic', base_meta=EXC)))
_patch = mock.patch.object(extract_ss, 'GAME', Path(_tmp.name))
def setUpModule(): _patch.start()
def tearDownModule(): _patch.stop(); _tmp.cleanup()
B, U = '27bc536b91a7089b9f2dac80720c08ba', '88d7f50b596192d406c20e6c395dfb80'


def log(base_meta='Invalid', tolerated=True, base_hash=EXC['program_sha256'], update_meta='Valid', damaged=False, update=True):
    lines = [f'[select] base game: {B} version 0 size 1 patch section False', f'[signature] base game meta: {base_meta} sha256 {EXC["meta_sha256"]}']
    lines += ['[tolerated] base game meta: invalid signature accepted by SHA-256; the version it declares is not authenticated'] if tolerated else []
    lines += ['[signature] base game program: Valid', f'[hash] base game program: computed {base_hash} expected {EXC["program_sha256"]}']
    if update:
        lines += [f'[select] update: {U} version 65536 size 1 patch section True', f'[signature] update meta: {update_meta} sha256 {"1"*64}',
                  '[signature] update program: Valid', f'[hash] update program: computed {"2"*64} expected {"2"*64}']
    lines += ['/x/ExeFS: 6 files 27.1 MB, 0 identical', '/x/RomFS: 3836 files 5943.3 MB, 0 identical']
    lines += ['[RED] hash check failed: /region_common/map/Field.bntx'] if damaged else []
    return '\n'.join(lines)+'\n'


class SummaryTests(unittest.TestCase):
    def summary(self, text, code=0, update=True):
        return extract_ss.summary(code, text, {}, update, EXC['meta_sha256'])

    def test_tolerated_base_meta_passes_and_is_recorded_apart(self):
        r = self.summary(log())
        self.assertEqual(r['status'], 'pass')
        self.assertEqual(r['signatures']['base game meta'], dict(result='Invalid', sha256=EXC['meta_sha256'], tolerated=True, exception='assets/integrity-exceptions.json'))
        self.assertEqual(r['signatures']['base game program']['result'], 'Valid')
        self.assertEqual(r['hashes']['base game']['meta'], 'base meta, signature not valid')
        self.assertEqual(r['metadata_authenticated'], dict(base_version_v0=False, update_version_65536=True))
        self.assertEqual(r['checks_performed'], ['content metas selected', 'header signatures', 'program SHA-256 against metas', 'every file read through ErrorOnInvalid'])

    def test_a_tolerated_meta_selects_only_the_named_program(self):
        self.assertEqual(self.summary(log(base_hash='3'*64))['status'], 'fail')

    def test_an_invalid_meta_without_the_tolerance_line_fails(self):
        self.assertEqual(self.summary(log(tolerated=False))['status'], 'fail')

    def test_an_update_meta_is_never_tolerated(self):
        self.assertEqual(self.summary(log(update_meta='Invalid'))['status'], 'fail')

    def test_a_damaged_file_or_an_exit_code_fails(self):
        self.assertEqual(self.summary(log(damaged=True))['status'], 'fail')
        self.assertEqual(self.summary(log(), code=3)['status'], 'fail')

    def test_a_missing_hash_line_fails(self):
        self.assertEqual(self.summary(log().replace('[hash] update', '[other] update'))['status'], 'fail')

    def test_base_alone(self):
        r = self.summary(log(update=False), update=False)
        self.assertEqual((r['status'], r['metadata_authenticated']), ('pass', dict(base_version_v0=False)))


class ToleranceTests(unittest.TestCase):
    def test_only_the_named_container_gets_the_tolerance(self):
        self.assertEqual(extract_ss.tolerated_meta(EXC['container_sha256']), EXC['meta_sha256'])
        self.assertIsNone(extract_ss.tolerated_meta('0'*64))

    def test_this_target_names_no_exception(self):
        self.assertIsNone(REAL)
        with mock.patch.object(extract_ss, 'GAME', GAME):
            self.assertIsNone(extract_ss.tolerated_meta(EXC['container_sha256']))
            self.assertEqual(extract_ss.summary(0, log(), {}, True, None)['status'], 'fail')   # a tolerance line with no exception never passes

    def test_the_build_never_skips_content_hashes(self):
        for path in (GAME/'scripts').glob('*.py'):
            self.assertNotIn('--skip-content-hash', path.read_text(), path.name)


if __name__ == '__main__':
    unittest.main()
