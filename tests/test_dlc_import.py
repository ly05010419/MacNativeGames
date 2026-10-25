"""Breath of the Wild 1.9.0 add-on content (DLC): games/botw/scripts/extract_dlc.py with synthetic encrypted NSPs, build_app --dlc
and step_dlc_botw. No game data, no real keys.

Run with the build environment's Python: python -m unittest discover -s tests -v
"""
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/totk/scripts'
BOTW_SCRIPTS = ROOT / 'games/botw/scripts'
sys.path[:0] = [str(ROOT), str(SCRIPTS)]
import build_app
from test_game_containers import AREA, HEADER, TITLEKEK, CONTENT, nca, partition, romfs, ticket
from test_game_selection import package, card_image, run_main, BOTW_BASE, BOTW_UPDATE, TOTK_BASE, TOTK_UPDATE, Stop

DLC1, DLC2 = '01007EF00011F001', '01007EF00011F002'


def dlc_nsp(path, title=DLC1, managed=False, flip=None):
    """A DLC NSP: one Data NCA (content type 5) with a synthetic RomFS, and a ticket. flip: byte offset in the NCA to damage."""
    data = bytearray(nca(managed, title=int(title, 16), content=5))
    if flip is not None:
        data[flip] ^= 1
    tik = bytearray(ticket())   # managed: its rights ID decrypts the NCA; else the key area does, and the ticket only names the title
    if not managed:
        tik[0x2a0:0x2b0] = bytes.fromhex(title) + (1).to_bytes(8, 'big')
    path.write_bytes(partition([('data.nca', bytes(data)), ('dlc.tik', bytes(tik))]))
    return path


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.keys = self.path / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in
                                      [('header_key', HEADER), ('key_area_key_application_00', AREA), ('titlekek_00', TITLEKEK)]))

    def extract(self, nsp, out='out'):
        return subprocess.run([sys.executable, BOTW_SCRIPTS / 'extract_dlc.py', nsp, '--keys', self.keys, '--output', self.path / out],
                              capture_output=True, text=True)


class ExtractDlc(Fixture):
    def test_dlc_romfs_is_imported_and_indexed(self):
        for managed, title, index in ((False, DLC1, 1), (True, DLC1, 1), (False, DLC2, 2)):
            with self.subTest(managed=managed, title=title):
                out = self.path / f'out{managed}{index}'
                p = self.extract(dlc_nsp(self.path / f'dlc{managed}{index}.nsp', title, managed), out.name)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                folder = out / str(index)
                self.assertEqual([x.name for x in out.iterdir()], [str(index)])
                self.assertEqual(sorted(x.name for x in folder.iterdir()), ['index.json', 'romfs.bin', 'runtime-index.bin'])
                self.assertEqual((folder / 'romfs.bin').read_bytes(), romfs())
                report = json.loads((folder / 'index.json').read_text())
                self.assertEqual((report['status'], report['title_id'], report['index'], report['romfs_size'], report['romfs_sha256']),
                                 ('pass', title, index, len(romfs()), hashlib.sha256(romfs()).hexdigest()))
                self.assertEqual(report['files'][0]['path'], 'icon_AmericanEnglish.dat')
                self.assertEqual(report['ivfc_blocks'], [1] * 6)
                data = (folder / 'runtime-index.bin').read_bytes()
                self.assertEqual(struct.unpack_from('<8sQ', data), (b'TOTKRFS1', 1))
                self.assertIn(b'icon_AmericanEnglish.dat', data)
                for secret in (CONTENT, TITLEKEK, HEADER, AREA):
                    self.assertNotIn(secret.hex(), p.stdout + p.stderr)

    def test_dlc_romfs_is_verified(self):
        # byte 10 of IVFC level 0 and of level 5 (the RomFS itself), counted from the NCA's first section at 0xc00
        for level in (0, 5):
            with self.subTest(level=level):
                p = self.extract(dlc_nsp(self.path / f'bad{level}.nsp', flip=0xc00 + level * 4096 + 10), f'bad{level}')
                self.assertNotEqual(p.returncode, 0)
                self.assertIn(f'IVFC level {level} block 0 failed', p.stderr)

    def test_other_titles_are_refused(self):
        for name, title in (('totk', '0100F2C0115B7001'), ('third', '01007EF00011F003')):
            p = self.extract(dlc_nsp(self.path / f'{name}.nsp', title), name)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn('not to Breath of the Wild add-on content', p.stderr)
            self.assertFalse((self.path / name / '1' / 'romfs.bin').exists())

    def test_a_program_is_not_a_dlc(self):
        nsp = self.path / 'program.nsp'
        nsp.write_bytes(partition([('program.nca', nca(title=int(BOTW_BASE, 16)))]))
        p = self.extract(nsp, 'program')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Expected one add-on content Data NCA', p.stderr)

    def test_output_must_be_new(self):
        (self.path / 'taken').mkdir()
        p = self.extract(dlc_nsp(self.path / 'dlc.nsp'), 'taken')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Output exists', p.stderr)


class DlcArguments(Fixture):
    """build_app --dlc: for Breath of the Wild with a separate 1.6.0 or 1.9.0 update; the files are checked before any long step."""

    def run_main(self, base, update=None, dlcs=(), game='botw', version='botw190'):
        return run_main(self, self.path, base, update, dlcs, game=game, stop='step_data_botw', version=version)

    def botw(self):
        return package(self.path, 'botw.nsp', BOTW_BASE), package(self.path, 'botw-u.nsp', BOTW_UPDATE)

    def test_dlc_reaches_the_data_step_once_each(self):
        base, update = self.botw()
        one, two = package(self.path, 'dlc1.nsp', DLC1), package(self.path, 'dlc2.nsp', DLC2)
        # the same file twice (also by another spelling) counts once; two files stay in order
        error, started = self.run_main(base, update, [one, one, self.path / '.' / 'dlc1.nsp', two])
        self.assertIsInstance(error, Stop)
        data = started['step_data_botw']
        self.assertEqual(data.call_args.kwargs['dlc'], [one.resolve(), two.resolve()])
        self.assertEqual(data.call_args.kwargs['version'], 'botw190')

    def test_1_6_0_update_takes_dlc_too(self):
        base, update = self.botw()
        one, two = package(self.path, 'dlc1.nsp', DLC1), package(self.path, 'dlc2.nsp', DLC2)
        error, started = self.run_main(base, update, [one, two], version='botw160')
        self.assertIsInstance(error, Stop)
        data = started['step_data_botw']
        self.assertEqual(data.call_args.kwargs['dlc'], [one.resolve(), two.resolve()])
        self.assertEqual(data.call_args.kwargs['version'], 'botw160')

    def test_no_dlc_is_an_empty_list(self):
        base, update = self.botw()
        _, started = self.run_main(base, update)
        self.assertEqual(started['step_data_botw'].call_args.kwargs['dlc'], [])

    def test_wrong_dlc_files_stop_before_any_step(self):
        base, update = self.botw()
        one = package(self.path, 'dlc1.nsp', DLC1)
        copy = package(self.path, 'dlc1-copy.nsp', DLC1)
        xci = self.path / 'dlc.xci'
        xci.write_bytes(bytes(0x100) + b'HEAD' + bytes(0x200))
        cases = (('same pack twice', [one, copy], 'same DLC pack'),
                 ('the base', [base], 'pass it as the game file'),
                 ('the update', [update], 'pass it as --update'),
                 ('another game', [package(self.path, 'totk-d.nsp', '0100F2C0115B7001')], 'Tears of the Kingdom'),
                 ('no pin', [package(self.path, 'dlc3.nsp', '01007EF00011F003')], '01007EF00011F003'),
                 ('an XCI', [xci], '--dlc must be an add-on content NSP'),
                 ('missing', [self.path / 'missing.nsp'], '--dlc must be an add-on content NSP'))
        for name, dlcs, message in cases:
            with self.subTest(name):
                error, started = self.run_main(base, update, dlcs)
                self.assertRegex(str(error), message)
                started['step_code_botw'].assert_not_called()

    def test_dlc_is_for_botw_with_base_nsp_and_update(self):
        dlc = package(self.path, 'dlc1.nsp', DLC1)
        base, update = self.botw()
        error, started = self.run_main(base, dlcs=[dlc])
        self.assertIn('needs the 1.9.0 update', str(error))
        error, started = self.run_main(card_image(self.path), dlcs=[dlc])
        self.assertIn('leave out --dlc', str(error))
        started['step_code_botw'].assert_not_called()
        error, started = self.run_main(package(self.path, 'totk.nsp', TOTK_BASE), package(self.path, 'totk-u.nsp', TOTK_UPDATE),
                                       [dlc], game='totk')
        self.assertIn('Tears of the Kingdom has no DLC support', str(error))
        started['step_code'].assert_not_called()

    def test_dlc_given_as_the_game_or_the_update_points_to_dlc(self):
        base, update = self.botw()
        dlc = package(self.path, 'dlc1.nsp', DLC1)
        error, _ = self.run_main(base, dlc)
        self.assertRegex(str(error), 'dlc.*pass it as --dlc')

    def test_identify_game_names_dlc(self):
        base, dlc = package(self.path, 'botw.nsp', BOTW_BASE), package(self.path, 'dlc1.nsp', DLC1)
        # identify_game: a DLC as the game file names --dlc, not "expected Tears of the Kingdom or Breath of the Wild"
        log = self.path / 'logs'
        log.mkdir()
        with mock.patch.object(build_app, 'run'), mock.patch.object(build_app, 'last_json', return_value={'titles': [DLC1]}):
            with self.assertRaisesRegex(build_app.BuildError, 'add-on content.*--dlc'):
                build_app.identify_game(self.path, dlc, self.keys, log)
            with self.assertRaisesRegex(build_app.BuildError, 'the game file holds'):
                build_app.identify_game(self.path, base, self.keys, log)
            with self.assertRaisesRegex(build_app.BuildError, 'does not support'):
                build_app.identify_game(self.path, package(self.path, 'totk-d.nsp', '0100F2C0115B7001'), self.keys, log)


class StepDlcBotw(Fixture):
    """step_dlc_botw imports, checks against the pins, reuses valid packs and drops packs that were not given."""

    def setUp(self):
        super().setUp()
        self.tree, self.logs = self.path / 'tree', self.path / 'logs'
        self.aoc = self.tree / 'extracted/botw190/aoc'
        self.nsps = {DLC1: dlc_nsp(self.path / 'dlc1.nsp', DLC1), DLC2: dlc_nsp(self.path / 'dlc2.nsp', DLC2)}
        sha = hashlib.sha256(romfs()).hexdigest()
        self.pins = {'1': dict(title_id=DLC1, size=len(romfs()), sha256=sha), '2': dict(title_id=DLC2, size=len(romfs()), sha256=sha)}
        self.calls = []
        for patcher in (mock.patch.object(build_app, 'run', side_effect=self.fake_run), mock.patch.object(build_app, 'say')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def fake_run(self, cmd, log, cwd=None, **kwargs):
        """Run the repository's extract_dlc.py instead of the work tree's copy."""
        self.calls.append(cmd)
        self.assertEqual(Path(cmd[1]).name, 'extract_dlc.py')
        p = subprocess.run([sys.executable, BOTW_SCRIPTS / 'extract_dlc.py', *cmd[2:]], capture_output=True, text=True)
        if p.returncode:
            raise build_app.BuildError(p.stderr)

    def step(self, *titles, pins=None):
        build_app.step_dlc_botw(self.tree, self.path / 'botw', [self.nsps[t] for t in titles], self.keys, self.logs,
                                pins or self.pins, self.aoc)

    def test_a_subset_is_imported_and_reused(self):
        self.step(DLC2)
        self.assertEqual([p.name for p in self.aoc.iterdir()], ['2'])   # pack 1 was not given: absent
        self.assertEqual(sorted(x.name for x in (self.aoc / '2').iterdir()), ['index.json', 'romfs.bin', 'runtime-index.bin'])
        self.assertFalse((self.tree / 'extracted/botw190/aoc-import').exists())
        self.step(DLC2)
        self.assertEqual(len(self.calls), 1, 'a valid pack is reused')
        flat = ' '.join(map(str, self.calls[0]))
        for secret in (CONTENT, TITLEKEK, HEADER, AREA):
            self.assertNotIn(secret.hex(), flat)

    def test_a_pack_not_given_this_time_is_removed(self):
        self.step(DLC1, DLC2)
        self.assertEqual(sorted(p.name for p in self.aoc.iterdir()), ['1', '2'])
        config = self.tree / 'build/botw190/aoc_config.h'
        config.parent.mkdir(parents=True)
        config.write_text('#define AOC_PRESENT_MASK 3\n')
        self.step(DLC1)
        self.assertEqual([p.name for p in self.aoc.iterdir()], ['1'])
        self.assertEqual(len(self.calls), 2, 'pack 1 was reused')
        self.assertFalse(config.exists(), 'the old mask goes with the removed pack')
        self.step()
        self.assertEqual(list(self.aoc.iterdir()), [])
        self.assertEqual(len(self.calls), 2)

    def test_a_damaged_pack_is_imported_again(self):
        self.step(DLC1)
        folder = self.aoc / '1'
        (folder / 'romfs.bin').write_bytes(b'short')
        self.step(DLC1)
        self.assertEqual(len(self.calls), 2)
        (folder / 'runtime-index.bin').unlink()
        self.step(DLC1)
        self.assertEqual(len(self.calls), 3)
        report = json.loads((folder / 'index.json').read_text())
        report['status'] = 'fail'
        (folder / 'index.json').write_text(json.dumps(report))
        self.step(DLC1)
        self.assertEqual(len(self.calls), 4)
        self.assertEqual((folder / 'romfs.bin').read_bytes(), romfs())

    def test_a_wrong_pin_stops_and_leaves_nothing_in_aoc(self):
        pins = {**self.pins, '1': dict(self.pins['1'], sha256='0' * 64)}
        with self.assertRaisesRegex(build_app.BuildError, 'add-on content 1: SHA-256 .* is not the expected'):
            self.step(DLC1, pins=pins)
        self.assertFalse((self.aoc / '1').exists())
        with self.assertRaisesRegex(build_app.BuildError, 'size'):
            self.step(DLC1, pins={**self.pins, '1': dict(self.pins['1'], size=1)})

    def test_a_failed_import_stops(self):
        bad = dlc_nsp(self.path / 'bad.nsp', flip=0xc00 + 10)
        with self.assertRaisesRegex(build_app.BuildError, 'IVFC level 0 block 0 failed'):
            build_app.step_dlc_botw(self.tree, self.path / 'botw', [bad], self.keys, self.logs, self.pins, self.aoc)
        self.assertFalse((self.tree / 'extracted/botw190/aoc-import').exists())


class StepDataRoutes(unittest.TestCase):
    """step_data_botw: a separate 1.6.0 or 1.9.0 update takes its add-on content from --dlc; the 1.6.0 card image still extracts
    both packs from itself."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tree = self.root / 'tree'

    def data(self, version, **kwargs):
        folder = self.tree / f'extracted/{version}/romfs'
        folder.mkdir(parents=True)
        expected = build_app.VERSIONS[version]
        (folder / 'romfs.bin').write_bytes(b'x' * 8)
        (folder / 'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256=expected['romfs']['sha256'])))
        pins = {**expected, 'romfs': {**expected['romfs'], 'size': 8}}
        commands = []

        def run(cmd, *args, **kwargs):
            commands.append(Path(cmd[1]).name)
            if 'extract_aoc_romfs.py' in str(cmd[1]):   # both packs, as the card image holds them
                for i, pin in expected['aoc'].items():
                    (self.tree / f'extracted/{version}/aoc' / i).mkdir(parents=True)
                    (self.tree / f'extracted/{version}/aoc' / i / 'index.json').write_text(json.dumps(dict(romfs_sha256=pin['sha256'])))

        with mock.patch.dict(build_app.VERSIONS, {version: pins}), mock.patch.object(build_app, 'fonts_valid', return_value=True), \
             mock.patch.object(build_app, 'build_hactool', return_value=Path('hactool')), mock.patch.object(build_app, 'say'), \
             mock.patch.object(build_app, 'step_dlc_botw') as step_dlc, \
             mock.patch.object(build_app, 'run', side_effect=run):
            build_app.step_data_botw(self.tree, self.root / 'botw', Path('base.nsp'), Path('prod.keys'), self.root / 'logs',
                                     version=version, **kwargs)
        return step_dlc, commands

    def test_1_9_0_hands_the_given_packs_to_step_dlc(self):
        dlc = [self.root / 'dlc2.nsp']
        step_dlc, commands = self.data('botw190', update=self.root / 'update.nsp', dlc=dlc)
        step_dlc.assert_called_once()
        self.assertEqual(step_dlc.call_args.args[2], dlc)
        self.assertEqual(sorted(step_dlc.call_args.args[5]), ['1', '2'])
        self.assertEqual(commands, [])   # nothing else is extracted

    def test_1_9_0_without_dlc_still_clears_old_packs(self):
        step_dlc, _ = self.data('botw190', update=self.root / 'update.nsp')
        self.assertEqual(step_dlc.call_args.args[2], ())

    def test_1_6_0_update_hands_the_given_packs_to_step_dlc(self):
        dlc = [self.root / 'dlc1.nsp', self.root / 'dlc2.nsp']
        step_dlc, commands = self.data('botw160', update=self.root / 'update.nsp', dlc=dlc)
        self.assertEqual(step_dlc.call_args.args[2], dlc)
        self.assertEqual(step_dlc.call_args.args[6], self.tree / 'extracted/botw160/aoc')
        self.assertEqual(commands, [])   # the card's extract_aoc_romfs.py does not run

    def test_card_image_extracts_its_own_packs(self):
        step_dlc, commands = self.data('botw160')
        step_dlc.assert_not_called()
        self.assertEqual(commands, ['extract_aoc_romfs.py'])


if __name__ == '__main__':
    unittest.main()
